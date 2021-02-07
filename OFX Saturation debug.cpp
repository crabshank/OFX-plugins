/*
Software License :

Copyright (c) 2003, The Open Effects Association Ltd. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name The Open Effects Association Ltd, nor the names of its
	  contributors may be used to endorse or promote products derived from this
	  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <cstring>
#include <stdexcept>
#include <new>
#include <iostream>
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxUtilities.H" // example support utils

#if defined __APPLE__ || defined linux || defined __FreeBSD__
#  define EXPORT __attribute__((visibility("default")))
#elif defined _WIN32
#  define EXPORT OfxExport
#else
#  error Not building on your operating system quite yet
#endif

#define rcptwoFiveFive 1.0/255.0

template <class T> inline T Maximum(T a, T b) { return a > b ? a : b; }
template <class T> inline T Minimum(T a, T b) { return a < b ? a : b; }

// pointers64 to various bits of the host
OfxHost* gHost;
OfxImageEffectSuiteV1* gEffectHost = 0;
OfxPropertySuiteV1* gPropHost = 0;
OfxParameterSuiteV1* gParamHost = 0;
OfxMemorySuiteV1* gMemoryHost = 0;
OfxMultiThreadSuiteV1* gThreadHost = 0;
OfxMessageSuiteV1* gMessageSuite = 0;
OfxInteractSuiteV1* gInteractHost = 0;

// some flags about the host's behaviour
int gHostSupportsMultipleBitDepths = false;

// private instance data type
struct MyInstanceData {


	// handles to the clips we deal with
	OfxImageClipHandle sourceClip;
	OfxImageClipHandle outputClip;

	OfxParamHandle saturationParam;
};

/* mandatory function to set up the host structures */

// Convinience wrapper to get private data 
static MyInstanceData*
getMyInstanceData(OfxImageEffectHandle effect)
{
	MyInstanceData* myData = (MyInstanceData*)ofxuGetEffectInstanceData(effect);
	return myData;
}

/** @brief Called at load */

//  instance construction
static OfxStatus
createInstance(OfxImageEffectHandle effect)
{
	// get a pointer to the effect properties
	OfxPropertySetHandle effectProps;
	gEffectHost->getPropertySet(effect, &effectProps);

	// get a pointer to the effect's parameter set
	OfxParamSetHandle paramSet;
	gEffectHost->getParamSet(effect, &paramSet);

	// make my private instance data
	MyInstanceData* myData = new MyInstanceData;

	// cache away param handles
	gParamHost->paramGetHandle(paramSet, "saturation", &myData->saturationParam, 0);

	// cache away clip handles
	gEffectHost->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &myData->sourceClip, 0);
	gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &myData->outputClip, 0);

	// set my private instance data
	gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void*)myData);

	return kOfxStatOK;
}

// instance destruction
static OfxStatus
destroyInstance(OfxImageEffectHandle effect)
{
	// get my instance data
	MyInstanceData* myData = getMyInstanceData(effect);

	// and delete it
	if (myData)
		delete myData;
	return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// rendering routines


// look up a pixel in the image, does bounds checking to see if it is in the image rectangle
template <class PIX> inline PIX*
pixelAddress(PIX* img, OfxRectI rect, int x, int y, int bytesPerLine)
{
	if (x < rect.x1 || x >= rect.x2 || y < rect.y1 || y > rect.y2)
		return 0;
	PIX* pix = (PIX*)(((char*)img) + (y - rect.y1) * bytesPerLine);
	pix += x - rect.x1;
	return pix;
}

////////////////////////////////////////////////////////////////////////////////
// base class to process images with
class Processor {
protected:
	OfxImageEffectHandle effect;
	double  saturation;
	void* srcV, * dstV;
	OfxRectI srcRect, dstRect;
	OfxRectI  window;
	int srcBytesPerLine, dstBytesPerLine;

public:
	Processor(OfxImageEffectHandle eff,
		double sat,
		void* src, OfxRectI sRect, int sBytesPerLine,
		void* dst, OfxRectI dRect, int dBytesPerLine,
		OfxRectI  win)
		: effect(eff)
		, saturation(sat)
		, srcV(src)
		, dstV(dst)
		, srcRect(sRect)
		, dstRect(dRect)
		, window(win)
		, srcBytesPerLine(sBytesPerLine)
		, dstBytesPerLine(dBytesPerLine)
	{}

	static void multiThreadProcessing(unsigned int threadId, unsigned int nThreads, void* arg);
	virtual void doProcessing(OfxRectI window) = 0;
	void process(void);
};

// function call once for each thread by the host
void
Processor::multiThreadProcessing(unsigned int threadId, unsigned int nThreads, void* arg)
{
	Processor* proc = (Processor*)arg;

	// slice the y range into the number of threads it has
	unsigned int dy = proc->window.y2 - proc->window.y1;

	unsigned int y1 = proc->window.y1 + threadId * dy / nThreads;
	unsigned int y2 = proc->window.y1 + Minimum((threadId + 1) * dy / nThreads, dy);

	OfxRectI win = proc->window;
	win.y1 = y1; win.y2 = y2;

	// and render that thread on each
	proc->doProcessing(win);
}

// function to kick off rendering across multiple CPUs
void
Processor::process(void)
{
	unsigned int nThreads;
	gThreadHost->multiThreadNumCPUs(&nThreads);
	gThreadHost->multiThread(multiThreadProcessing, nThreads, (void*)this);
}

// template to do the RGBA processing
template <class PIX, int max>
class ProcessRGBA : public Processor {
public:
	ProcessRGBA(OfxImageEffectHandle eff,
		double sat,
		void* src, OfxRectI sRect, int sBytesPerLine,
		void* dst, OfxRectI dRect, int dBytesPerLine,
		OfxRectI  win)
		: Processor(eff,
			sat,
			src, sRect, sBytesPerLine,
			dst, dRect, dBytesPerLine,
			win)
	{
	}

	void doProcessing(OfxRectI procWindow)
	{
		PIX* src = (PIX*)srcV;
		PIX* dst = (PIX*)dstV;

		for (int y = procWindow.y1; y < procWindow.y2; y++) {
			if (gEffectHost->abort(effect)) break;

			PIX* dstPix = pixelAddress(dst, dstRect, procWindow.x1, y, dstBytesPerLine);

			for (int x = procWindow.x1; x < procWindow.x2; x++) {

				PIX* srcPix = 0;
				if (src)
					srcPix = pixelAddress(src, srcRect, x, y, srcBytesPerLine);


					double OG_RGB[3] = { (double)(srcPix->r) * rcptwoFiveFive,(double)(srcPix->g) * rcptwoFiveFive ,(double)(srcPix->b) * rcptwoFiveFive };

					double mx = fmax(OG_RGB[0], fmax(OG_RGB[1], OG_RGB[2]));
					double mn = fmin(OG_RGB[0], fmin(OG_RGB[1], OG_RGB[2]));
					double sat = (mx==0)?0:(mx-mn)/mx;
					
					dstPix->r =(sat<= saturation)?0:fmax(fmin(round(OG_RGB[0] * 255), 255), 0);
					dstPix->g =(sat<= saturation)?0:fmax(fmin(round(OG_RGB[1] * 255), 255), 0);
					dstPix->b =(sat<= saturation)?0:fmax(fmin(round(OG_RGB[2] * 255), 255), 0);

				dstPix++;
			}


		}
	}
};

// the process code  that the host sees
static OfxStatus render(OfxImageEffectHandle effect,
	OfxPropertySetHandle inArgs,
	OfxPropertySetHandle outArgs)
{
	// get the render window and the time from the inArgs
	OfxTime time;
	OfxRectI renderWindow;
	OfxStatus status = kOfxStatOK;

	gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
	gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

	// retrieve any instance data associated with this effect
	MyInstanceData* myData = getMyInstanceData(effect);

	// property handles and members of each image
	// in reality, we would put this in a struct as the C++ support layer does

	OfxPropertySetHandle sourceImg = NULL, outputImg = NULL;
	int srcRowBytes = 0, srcBitDepth, dstRowBytes, dstBitDepth;
	bool srcIsAlpha, dstIsAlpha;
	OfxRectI dstRect, srcRect = { 0 };
	void* src = NULL, * dst;

	try {
		outputImg = ofxuGetImage(myData->outputClip, time, dstRowBytes, dstBitDepth, dstIsAlpha, dstRect, dst);
		if (outputImg == NULL) throw OfxuNoImageException();

		sourceImg = ofxuGetImage(myData->sourceClip, time, srcRowBytes, srcBitDepth, srcIsAlpha, srcRect, src);
		if (sourceImg == NULL) throw OfxuNoImageException();

		// get the saturation of it
		double saturation;
		gParamHost->paramGetValueAtTime(myData->saturationParam, time, &saturation);


		if ((srcBitDepth == 8) && (dstBitDepth == 8)) {
			ProcessRGBA<OfxRGBAColourB, 255> fred(effect, saturation,   //	ProcessRGBA<OfxRGBAColourB, 255, 0> fred(effect, rectI, saturation, unpremultiplied,
				src, srcRect, srcRowBytes,
				dst, dstRect, dstRowBytes,
				renderWindow);
			fred.process();
		}

		// do the rendering
	}
	catch (OfxuNoImageException& ex) {
		// if we were interrupted, the failed fetch is fine, just return kOfxStatOK
		// otherwise, something wierd happened
		if (!gEffectHost->abort(effect)) {
			status = kOfxStatFailed;
		}
	}

	// release the data pointers
	if (sourceImg)
		gEffectHost->clipReleaseImage(sourceImg);
	if (outputImg)
		gEffectHost->clipReleaseImage(outputImg);

	return status;
}

//  describe the plugin in context
static OfxStatus
describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
	OfxPropertySetHandle clipProps;
	gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clipProps);
	gPropHost->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);



	gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &clipProps);
	// set the component types we can handle on our main input
	gPropHost->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);


	////////////////////////////////////////////////////////////////////////////////
	// define the parameters for this context

	// get a pointer to the effect's parameter set
	OfxParamSetHandle paramSet;
	gEffectHost->getParamSet(effect, &paramSet);

	// our 2 corners are normalised spatial 2D doubles
	OfxPropertySetHandle paramProps;

	// make an rgba saturation parameter
	gParamHost->paramDefine(paramSet, kOfxParamTypeDouble, "saturation", &paramProps);

	gPropHost->propSetDouble(paramProps, kOfxParamPropDefault, 0, 0.02);

	gPropHost->propSetString(paramProps, kOfxParamPropHint, 0, "The saturation at which and below pixels will turn black");
	gPropHost->propSetString(paramProps, kOfxParamPropScriptName, 0, "saturation");
	gPropHost->propSetString(paramProps, kOfxPropLabel, 0, "Saturation");

	return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin's description routine
static OfxStatus
describe(OfxImageEffectHandle effect)
{
	// first fetch the host APIs, this cannot be done before this call
	OfxStatus stat;
	if ((stat = ofxuFetchHostSuites()) != kOfxStatOK)
		return stat;

	OfxPropertySetHandle effectProps;
	gEffectHost->getPropertySet(effect, &effectProps);

	// say we cannot support multiple pixel depths and let the clip preferences action deal with it all.
	gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);

	// set the bit depths the plugin can handle
	gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);

	// set plugin label and the group it belongs to
	gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "Saturation debug");
	gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "Saturation debug");

	// define the contexts we can be used in
	gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);

	return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// The main function
static OfxStatus
pluginMain(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
	try {
		// cast to appropriate type
		OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;

		if (strcmp(action, kOfxActionDescribe) == 0) {
			return describe(effect);
		}
		else if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
			return describeInContext(effect, inArgs);
		}
		else if (strcmp(action, kOfxActionCreateInstance) == 0) {
			return createInstance(effect);
		}
		else if (strcmp(action, kOfxActionDestroyInstance) == 0) {
			return destroyInstance(effect);
		}
		else if (strcmp(action, kOfxImageEffectActionRender) == 0) {
			return render(effect, inArgs, outArgs);
		}
	}
	catch (std::bad_alloc) {
		// catch memory
		//std::cout << "OFX Plugin Memory error." << std::endl;
		return kOfxStatErrMemory;
	}
	catch (const std::exception& e) {
		// standard exceptions
		//std::cout << "OFX Plugin error: " << e.what() << std::endl;
		return kOfxStatErrUnknown;
	}
	catch (int err) {
		// ho hum, gone wrong somehow
		return err;
	}
	catch (...) {
		// everything else
		//std::cout << "OFX Plugin error" << std::endl;
		return kOfxStatErrUnknown;
	}

	// other actions to take the default value
	return kOfxStatReplyDefault;
}

// function to set the host structure
static void
setHostFunc(OfxHost* hostStruct)
{
	gHost = hostStruct;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin struct 
static OfxPlugin basicPlugin =
{
  kOfxImageEffectPluginApi,
  1,
  "uk.co.thefoundry.GeneratorExample",
  1,
  0,
  setHostFunc,
  pluginMain
};

// the two mandated functions
EXPORT OfxPlugin*
OfxGetPlugin(int nth)
{
	if (nth == 0)
		return &basicPlugin;
	return 0;
}

EXPORT int
OfxGetNumberOfPlugins(void)
{
	return 1;
}

/*
Software License :

Copyright (c) 2012, The Open Effects Association Ltd. All rights reserved.

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


/*
   Direct GPU processing using OpenGL
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#include <map>

#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"

#include "ofxUtilities.H" // example support utils

// pointers64 to various bits of the host
OfxHost               *gHost;
OfxImageEffectSuiteV1 *gEffectHost = 0;
OfxPropertySuiteV1    *gPropHost = 0;
OfxParameterSuiteV1   *gParamHost = 0;
OfxMemorySuiteV1      *gMemoryHost = 0;
OfxMultiThreadSuiteV1 *gThreadHost = 0;
OfxMessageSuiteV1     *gMessageSuite = 0;
OfxInteractSuiteV1    *gInteractHost = 0;

// some flags about the host's behaviour
int gHostSupportsMultipleBitDepths = false;
int gHostSupportsOpenCL = false;

#define CHECK_STATUS(args) check_status_fun args

static void
check_status_fun(int status, int expected, const char *name)
{
  if (status != expected) {
    fprintf(stderr, "OFX error in %s: expected status %d, got %d\n",
	    name, expected, status);
  }
}

#define DPRINT(args) print_dbg args
void print_dbg(const char *fmt, ...)
{
  char msg[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, 1023, fmt, ap);
  fwrite(msg, sizeof(char), strlen(msg), stderr);
  fflush(stderr);
#ifdef _WIN32
  OutputDebugString(msg);
#endif
  va_end(ap);
}

// private instance data type
struct MyInstanceData {
  bool isGeneralEffect;

  // handles to the clips we deal with
  OfxImageClipHandle sourceClip;
  OfxImageClipHandle outputClip;

  // handles to our parameters
  OfxParamHandle rGainParam;
  OfxParamHandle gGainParam;
  OfxParamHandle bGainParam;
};

/* mandatory function to set up the host structures */


// Convinience wrapper to get private data
static MyInstanceData *
getMyInstanceData( OfxImageEffectHandle effect)
{
  // get the property handle for the plugin
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // get my data pointer out of that
  MyInstanceData *myData = 0;
  gPropHost->propGetPointer(effectProps,  kOfxPropInstanceData, 0,
			    (void **) &myData);
  return myData;
}

/** @brief Called at load */
static OfxStatus
onLoad(void)
{
  return kOfxStatOK;
}

/** @brief Called before unload */
static OfxStatus
onUnLoad(void)
{
  return kOfxStatOK;
}

//  instance construction
static OfxStatus
createInstance( OfxImageEffectHandle effect)
{
  // get a pointer to the effect properties
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // get a pointer to the effect's parameter set
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  // make my private instance data
  MyInstanceData *myData = new MyInstanceData;
  const char *context = NULL;

  // is this instance a general effect ?
  gPropHost->propGetString(effectProps, kOfxImageEffectPropContext, 0,  &context);
  myData->isGeneralEffect = context && (strcmp(context, kOfxImageEffectContextGeneral) == 0);

  // cache away our param handles
  gParamHost->paramGetHandle(paramSet, "R Gain", &myData->rGainParam, 0);
  gParamHost->paramGetHandle(paramSet, "G Gain", &myData->gGainParam, 0);
  gParamHost->paramGetHandle(paramSet, "B Gain", &myData->bGainParam, 0);

  // cache away our clip handles
  gEffectHost->clipGetHandle(effect, "Source", &myData->sourceClip, 0);
  gEffectHost->clipGetHandle(effect, "Output", &myData->outputClip, 0);

  // set my private instance data
  gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) myData);

  return kOfxStatOK;
}

// instance destruction
static OfxStatus
destroyInstance( OfxImageEffectHandle  effect)
{
  // get my instance data
  MyInstanceData *myData = getMyInstanceData(effect);

  // and delete it
  if(myData)
    delete myData;
  return kOfxStatOK;
}

// tells the host what region we are capable of filling
OfxStatus
getSpatialRoD( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);

  OfxTime time;
  gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

  // my RoD is the same as my input's
  OfxRectD rod;
  gEffectHost->clipGetRegionOfDefinition(myData->sourceClip, time, &rod);

  // set the rod in the out args
  gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, &rod.x1);

  return kOfxStatOK;
}

// tells the host how much of the input we need to fill the given window
OfxStatus
getSpatialRoI( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // get the RoI the effect is interested in from inArgs
  OfxRectD roi;
  gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRegionOfInterest, 4, &roi.x1);

  // the input needed is the same as the output, so set that on the source clip
  gPropHost->propSetDoubleN(outArgs, "OfxImageClipPropRoI_Source", 4, &roi.x1);

  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);

  return kOfxStatOK;
}

// Tells the host how many frames we can fill, only called in the general context.
// This is actually redundant as this is the default behaviour, but for illustrative
// purposes.
OfxStatus
getTemporalDomain( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  MyInstanceData *myData = getMyInstanceData(effect);

  double sourceRange[2];

  // get the frame range of the source clip
  OfxPropertySetHandle props; gEffectHost->clipGetPropertySet(myData->sourceClip, &props);
  gPropHost->propGetDoubleN(props, kOfxImageEffectPropFrameRange, 2, sourceRange);

  // set it on the out args
  gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropFrameRange, 2, sourceRange);

  return kOfxStatOK;
}


// Set our clip preferences
static OfxStatus
getClipPreferences( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);

  // get the component type and bit depth of our main input
  int  bitDepth;
  bool isRGBA;
  ofxuClipGetFormat(myData->sourceClip, bitDepth, isRGBA, true); // get the unmapped clip component

  // get the strings used to label the various bit depths
  const char *bitDepthStr = bitDepth == 8 ? kOfxBitDepthByte : (bitDepth == 16 ? kOfxBitDepthShort : kOfxBitDepthFloat);
  const char *componentStr = isRGBA ? kOfxImageComponentRGBA : kOfxImageComponentAlpha;

  // set out output to be the same same as the input, component and bitdepth
  gPropHost->propSetString(outArgs, "OfxImageClipPropComponents_Output", 0, componentStr);
  if(gHostSupportsMultipleBitDepths)
    gPropHost->propSetString(outArgs, "OfxImageClipPropDepth_Output", 0, bitDepthStr);

  return kOfxStatOK;
}

// are the settings of the effect performing an identity operation
static OfxStatus
isIdentity( OfxImageEffectHandle  effect,
	    OfxPropertySetHandle inArgs,
	    OfxPropertySetHandle outArgs)
{
  // In this case do the default, which in this case is to render
  return kOfxStatReplyDefault;
}

////////////////////////////////////////////////////////////////////////////////
// function called when the instance has been changed by anything
static OfxStatus
instanceChanged( OfxImageEffectHandle  effect,
		 OfxPropertySetHandle inArgs,
		 OfxPropertySetHandle outArgs)
{
  // don't trap any others
  return kOfxStatReplyDefault;
}

////////////////////////////////////////////////////////////////////////////////
// rendering routines

const char *KernelSource = "\n" \
"__kernel void gain(                                                    \n" \
"   int width,                                                          \n" \
"   int height,                                                         \n" \
"   float rGain,                                                        \n" \
"   float gGain,                                                        \n" \
"   float bGain,                                                        \n" \
"   __global float* input,                                              \n" \
"   __global float* output)                                             \n" \
"{                                                                      \n" \
"   int x = get_global_id(0);                                           \n" \
"   int y = get_global_id(1);                                           \n" \
"   if ((x < width) && (y < height))                                    \n" \
"   {                                                                   \n" \
"       int index = (y * width + x) * 4;                                \n" \
"       output[index + 0] = input[index + 0] * rGain;                   \n" \
"       output[index + 1] = input[index + 1] * gGain;                   \n" \
"       output[index + 2] = input[index + 2] * bGain;                   \n" \
"       output[index + 3] = input[index + 3];                           \n" \
"   }                                                                   \n" \
"}                                                                      \n" \
"\n";

static void CheckError(cl_int p_Error, const char* p_Msg)
{
    if (p_Error != CL_SUCCESS)
    {
        DPRINT(("%s [%d]\n", p_Msg, p_Error));
    }
}

static cl_kernel CreateKernel(cl_context p_Context)
{
    cl_int error;

    cl_program program = clCreateProgramWithSource(p_Context, 1, (const char **)&KernelSource, NULL, &error);
    CheckError(error, "Unable to create program");

    error = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    CheckError(error, "Unable to build program");

    cl_kernel kernel = clCreateKernel(program, "gain", &error);
    CheckError(error, "Unable to create kernel");

    return kernel;
}

static cl_context GetContext(cl_device_id& p_DeviceId)
{
    static cl_context clContext = NULL;

    if (clContext == NULL)
    {
        clGetDeviceIDs(NULL, CL_DEVICE_TYPE_GPU, 1, &p_DeviceId, NULL);
        clContext = clCreateContext(NULL, 1, &p_DeviceId, NULL, NULL, NULL);
    }
    else
    {
        clGetContextInfo(clContext, CL_CONTEXT_DEVICES, sizeof(cl_device_id), &p_DeviceId, NULL);
    }

    return clContext;
}

static cl_kernel GetKernel(cl_context p_Context)
{
    static std::map<cl_context, cl_kernel> contextKernelMap;

    cl_kernel kernel;

    std::map<cl_context, cl_kernel>::iterator iter = contextKernelMap.find(p_Context);
    if (iter == contextKernelMap.end())
    {
        kernel = CreateKernel(p_Context);
        contextKernelMap[p_Context] = kernel;
    }
    else
    {
        kernel = iter->second;
    }

    return kernel;
}

static void RunKernel(cl_command_queue p_CmdQ, cl_device_id p_DeviceId, cl_kernel p_Kernel, int p_Width, int p_Height,
                      float p_RGain, float p_GGain, float p_BGain, cl_mem p_Input, cl_mem p_Output)
{
    cl_int error;
    error = clSetKernelArg(p_Kernel, 0, sizeof(int), &p_Width);
    error |= clSetKernelArg(p_Kernel, 1, sizeof(int), &p_Height);
    error |= clSetKernelArg(p_Kernel, 2, sizeof(float), &p_RGain);
    error |= clSetKernelArg(p_Kernel, 3, sizeof(float), &p_GGain);
    error |= clSetKernelArg(p_Kernel, 4, sizeof(float), &p_BGain);
    error |= clSetKernelArg(p_Kernel, 5, sizeof(cl_mem), &p_Input);
    error |= clSetKernelArg(p_Kernel, 6, sizeof(cl_mem), &p_Output);
    CheckError(error, "Unable to set kernel arguments");

    size_t localWorkSize[2], globalWorkSize[2];
    clGetKernelWorkGroupInfo(p_Kernel, p_DeviceId, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), localWorkSize, NULL);
    localWorkSize[1] = 1;
    globalWorkSize[0] = ((p_Width + localWorkSize[0] - 1) / localWorkSize[0]) * localWorkSize[0];
    globalWorkSize[1] = p_Height;

    clEnqueueNDRangeKernel(p_CmdQ, p_Kernel, 2, NULL, globalWorkSize, localWorkSize, 0, NULL, NULL);
}

// the process code  that the host sees
static OfxStatus render( OfxImageEffectHandle  instance,
                         OfxPropertySetHandle inArgs,
                         OfxPropertySetHandle outArgs)
{
  // get the render window and the time from the inArgs
  OfxTime time;
  OfxRectI renderWindow;
  OfxStatus status = kOfxStatOK;

  gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

  // Retrieve instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(instance);

  // property handles and members of each image
  OfxPropertySetHandle sourceImg = NULL, outputImg = NULL;
  int srcRowBytes, srcBitDepth, dstRowBytes, dstBitDepth;
  bool srcIsAlpha, dstIsAlpha;
  OfxRectI dstRect, srcRect;
  void *src, *dst;

  DPRINT(("Render: window = [%d, %d - %d, %d]\n",
	  renderWindow.x1, renderWindow.y1,
	  renderWindow.x2, renderWindow.y2));

  int isOpenCLEnabled = 0;
  if (gHostSupportsOpenCL)
  {
      gPropHost->propGetInt(inArgs, kOfxImageEffectPropOpenCLEnabled, 0, &isOpenCLEnabled);
      DPRINT(("render: OpenCL rendering %s\n", isOpenCLEnabled ? "enabled" : "DISABLED"));
  }

  cl_context clContext = NULL;
  cl_command_queue cmdQ = NULL;
  cl_device_id deviceId = NULL;
  if (isOpenCLEnabled)
  {
      void* voidPtrCmdQ;
      gPropHost->propGetPointer(inArgs, kOfxImageEffectPropOpenCLCommandQueue, 0, &voidPtrCmdQ);
      cmdQ = reinterpret_cast<cl_command_queue>(voidPtrCmdQ);

      clGetCommandQueueInfo(cmdQ, CL_QUEUE_CONTEXT, sizeof(cl_context), &clContext, NULL);
      clGetCommandQueueInfo(cmdQ, CL_QUEUE_DEVICE, sizeof(cl_device_id), &deviceId, NULL);
  }
  else
  {
      clContext = GetContext(deviceId);
      cmdQ = clCreateCommandQueue(clContext, deviceId, 0, NULL);
  }

  char deviceName[128];
  clGetDeviceInfo(deviceId, CL_DEVICE_NAME, 128, deviceName, NULL);
  DPRINT(("Using %s for plugin\n", deviceName));

  cl_kernel kernel = GetKernel(clContext);

  // get the source image
  sourceImg = ofxuGetImage(myData->sourceClip, time, srcRowBytes, srcBitDepth, srcIsAlpha, srcRect, src);

  // get the output image
  outputImg = ofxuGetImage(myData->outputClip, time, dstRowBytes, dstBitDepth, dstIsAlpha, dstRect, dst);

  // get the scale parameter
  double rGain = 1, gGain = 1, bGain = 1;
  gParamHost->paramGetValueAtTime(myData->rGainParam, time, &rGain);
  gParamHost->paramGetValueAtTime(myData->gGainParam, time, &gGain);
  gParamHost->paramGetValueAtTime(myData->bGainParam, time, &bGain);
  DPRINT(("Gain(%f %f %f)\n", rGain, gGain, bGain));

  float w = (renderWindow.x2 - renderWindow.x1);
  float h = (renderWindow.y2 - renderWindow.y1);

  const size_t rowSize = w * 4 * sizeof(float);

  if (isOpenCLEnabled)
  {
      DPRINT(("Using OpenCL transfers (same device)\n"));

      RunKernel(cmdQ, deviceId, kernel, w, h, rGain, gGain, bGain, (cl_mem)src, (cl_mem)dst);
  }
  else
  {
      DPRINT(("Using CPU transfers\n"));

      const size_t bufferSize = w * h * 4 * sizeof(float);

      // Allocate the temporary buffers on the plugin device
      cl_mem inBuffer = clCreateBuffer(clContext, CL_MEM_READ_ONLY, bufferSize, NULL, NULL);
      cl_mem outBuffer = clCreateBuffer(clContext, CL_MEM_WRITE_ONLY, bufferSize, NULL, NULL);

      // Copy the buffer from the CPU to the plugin device
      clEnqueueWriteBuffer(cmdQ, inBuffer, CL_TRUE, 0, bufferSize, src, 0, NULL, NULL);

      RunKernel(cmdQ, deviceId, kernel, w, h, rGain, gGain, bGain, inBuffer, outBuffer);

      // Copy the buffer from the plugin device to the CPU
      clEnqueueReadBuffer(cmdQ, outBuffer, CL_TRUE, 0, bufferSize, dst, 0, NULL, NULL);

      clFinish(cmdQ);

      // Free the temporary buffers on the plugin device
      clReleaseMemObject(inBuffer);
      clReleaseMemObject(outBuffer);
  }

  if (sourceImg)
  {
      gEffectHost->clipReleaseImage(sourceImg);
  }

  if (outputImg)
  {
      gEffectHost->clipReleaseImage(outputImg);
  }

  return status;
}

// convience function to define parameters
static void
defineParam( OfxParamSetHandle effectParams,
	     const char *name,
	     const char *label,
	     const char *scriptName,
	     const char *hint,
	     const char *parent)
{
  OfxParamHandle param;
  OfxPropertySetHandle props;
  gParamHost->paramDefine(effectParams, kOfxParamTypeDouble, name, &props);

  // say we are a scaling parameter
  gPropHost->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeScale);
  gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 1.0);
  gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 0.01);
  gPropHost->propSetDouble(props, kOfxParamPropDisplayMin, 0, 0.01);
  gPropHost->propSetDouble(props, kOfxParamPropDisplayMax, 0, 2.0);
  gPropHost->propSetDouble(props, kOfxParamPropIncrement, 0, 0.01);
  gPropHost->propSetString(props, kOfxParamPropHint, 0, hint);
  gPropHost->propSetString(props, kOfxParamPropScriptName, 0, scriptName);
  gPropHost->propSetString(props, kOfxPropLabel, 0, label);
  if(parent)
    gPropHost->propSetString(props, kOfxParamPropParent, 0, parent);
}

//  describe the plugin in context
static OfxStatus
describeInContext( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs)
{
  // get the context from the inArgs handle
  const char *context;
  gPropHost->propGetString(inArgs, kOfxImageEffectPropContext, 0, &context);
  bool isGeneralContext = strcmp(context, kOfxImageEffectContextGeneral) == 0;

  OfxPropertySetHandle props;
  // define the single output clip in both contexts
  gEffectHost->clipDefine(effect, "Output", &props);

  // set the component types we can handle on out output
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);

  // define the single source clip in both contexts
  gEffectHost->clipDefine(effect, "Source", &props);

  // set the component types we can handle on our main input
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);

  ////////////////////////////////////////////////////////////////////////////////
  // define the parameters for this context
  // fetch the parameter set from the effect
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  defineParam(paramSet, "R Gain", "R Gain", "R Gain",
          "Red Gain", 0);
  defineParam(paramSet, "G Gain", "G Gain", "G Gain",
          "Green Gain", 0);
  defineParam(paramSet, "B Gain", "B Gain", "B Gain",
          "Blue Gain", 0);

  // make a page of controls and add my parameters to it
  OfxParamHandle page;
  gParamHost->paramDefine(paramSet, kOfxParamTypePage, "Main", &props);
  gPropHost->propSetString(props, kOfxParamPropPageChild, 0, "R Gain");
  gPropHost->propSetString(props, kOfxParamPropPageChild, 1, "G Gain");
  gPropHost->propSetString(props, kOfxParamPropPageChild, 2, "B Gain");
  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin's description routine
static OfxStatus
describe(OfxImageEffectHandle  effect)
{
  // first fetch the host APIs, this cannot be done before this call
  OfxStatus stat;
  if((stat = ofxuFetchHostSuites()) != kOfxStatOK)
    return stat;

  // record a few host features
  gPropHost->propGetInt(gHost->host, kOfxImageEffectPropSupportsMultipleClipDepths, 0, &gHostSupportsMultipleBitDepths);

  // get the property handle for the plugin
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // We can render both fields in a fielded images in one hit if there is no animation
  // So set the flag that allows us to do this
  gPropHost->propSetInt(effectProps, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);

  // say we can support multiple pixel depths and let the clip preferences action deal with it all.
  gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);

  // set the bit depths the plugin can handle
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);

  // set some labels and the group it belongs to
  gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "Resolve Simple Gain (OpenCL)");
  gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "Resolve Simple Gain");

  // define the contexts we can be used in
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);

  // we support OpenCL rendering
  gPropHost->propSetString(effectProps, kOfxImageEffectPropOpenCLRenderSupported, 0, "true");

  {
    const char *s = "<undefined>";
    stat = gPropHost->propGetString(gHost->host, kOfxImageEffectPropOpenCLRenderSupported, 0, &s);
    DPRINT(("Host has OpenCL render support: %s (stat=%d)\n", s, stat));
    gHostSupportsOpenCL = stat == 0 && !strcmp(s, "true");
  }

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// The main function
static OfxStatus
pluginMain(const char *action,  const void *handle, OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // cast to appropriate type
  OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

  if(strcmp(action, kOfxActionDescribe) == 0) {
    return describe(effect);
  }
  else if(strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
    return describeInContext(effect, inArgs);
  }
  else if(strcmp(action, kOfxActionLoad) == 0) {
    return onLoad();
  }
  else if(strcmp(action, kOfxActionUnload) == 0) {
    return onUnLoad();
  }
  else if(strcmp(action, kOfxActionCreateInstance) == 0) {
    return createInstance(effect);
  }
  else if(strcmp(action, kOfxActionDestroyInstance) == 0) {
    return destroyInstance(effect);
  }
  else if(strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
    return isIdentity(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionRender) == 0) {
    return render(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
    return getSpatialRoD(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0) {
    return getSpatialRoI(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
    return getClipPreferences(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxActionInstanceChanged) == 0) {
    return instanceChanged(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetTimeDomain) == 0) {
    return getTemporalDomain(effect, inArgs, outArgs);
  }


  // other actions to take the default value
  return kOfxStatReplyDefault;
}

// function to set the host structure
static void
setHostFunc(OfxHost *hostStruct)
{
  gHost         = hostStruct;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin struct
static OfxPlugin basicPlugin =
{
  kOfxImageEffectPluginApi,
  1,
  "com.blackmagicdesign.ResolveSimpleGainOpenCLPlugin",
  1,
  0,
  setHostFunc,
  pluginMain
};

// the two mandated functions
OfxPlugin *
OfxGetPlugin(int nth)
{
  if(nth == 0)
    return &basicPlugin;
  return 0;
}

int
OfxGetNumberOfPlugins(void)
{
  return 1;
}

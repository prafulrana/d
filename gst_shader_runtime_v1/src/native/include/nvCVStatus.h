/*###############################################################################
#
# Copyright 2020 NVIDIA Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
###############################################################################*/

#ifndef __NVCVSTATUS_H__
#define __NVCVSTATUS_H__

#ifndef NvCV_API
  #ifdef _WIN32
    #ifdef NVCV_API_EXPORT
      #define NvCV_API __declspec(dllexport) __cdecl
    #else
      #define NvCV_API
    #endif
  #else //if linux
    #define NvCV_API   // TODO: Linux code goes here
  #endif // _WIN32 or linux
#endif //NvCV_API


#ifdef __cplusplus
extern "C" {
#endif // ___cplusplus


//! Status codes returned from APIs.
typedef enum NvCV_Status {
  NVCV_SUCCESS                   = 0,    //!< The procedure returned successfully.
  NVCV_ERR_GENERAL               = -1,   //!< An otherwise unspecified error has occurred.
  NVCV_ERR_UNIMPLEMENTED         = -2,   //!< The requested feature is not yet implemented.
  NVCV_ERR_MEMORY                = -3,   //!< There is not enough memory for the requested operation.
  NVCV_ERR_EFFECT                = -4,   //!< An invalid effect handle has been supplied.
  NVCV_ERR_SELECTOR              = -5,   //!< The given parameter selector and type is not valid in this effect filter.
  NVCV_ERR_BUFFER                = -6,   //!< An image buffer has not been specified.
  NVCV_ERR_PARAMETER             = -7,   //!< An invalid parameter value has been supplied for this effect+selector.
  NVCV_ERR_MISMATCH              = -8,   //!< Some parameters are not appropriately matched.
  NVCV_ERR_PIXELFORMAT           = -9,   //!< The specified pixel format is not accommodated.
  NVCV_ERR_MODEL                 = -10,  //!< Error while loading the TRT model.
  NVCV_ERR_LIBRARY               = -11,  //!< Error loading the dynamic library.
  NVCV_ERR_INITIALIZATION        = -12,  //!< The effect has not been properly initialized.
  NVCV_ERR_FILE                  = -13,  //!< The file could not be found.
  NVCV_ERR_FEATURENOTFOUND       = -14,  //!< The requested feature or capability was not found
  NVCV_ERR_MISSINGINPUT          = -15,  //!< A required parameter was not set
  NVCV_ERR_RESOLUTION            = -16,  //!< The specified image resolution is not supported.
  NVCV_ERR_UNSUPPORTEDGPU        = -17,  //!< The GPU is not supported
  NVCV_ERR_WRONGGPU              = -18,  //!< The current GPU is not the one selected.
  NVCV_ERR_UNSUPPORTEDDRIVER     = -19,  //!< The currently installed graphics driver is not supported
  NVCV_ERR_MODELDEPENDENCIES     = -20,  //!< There is no model with dependencies that match this system
  NVCV_ERR_PARSE                 = -21,  //!< There has been a parsing or syntax error while reading a file
  NVCV_ERR_MODELSUBSTITUTION     = -22,  //!< The specified model does not exist and has been substituted.
  NVCV_ERR_READ                  = -23,  //!< An error occurred while reading a file.
  NVCV_ERR_WRITE                 = -24,  //!< An error occurred while writing a file.
  NVCV_ERR_PARAMREADONLY         = -25,  //!< The selected parameter is read-only.
  NVCV_ERR_TRT_ENQUEUE           = -26,  //!< TensorRT enqueue failed.
  NVCV_ERR_TRT_BINDINGS          = -27,  //!< Unexpected TensorRT bindings.
  NVCV_ERR_TRT_CONTEXT           = -28,  //!< An error occurred while creating a TensorRT context.
  NVCV_ERR_TRT_INFER             = -29,  //!< The was a problem creating the inference engine.
  NVCV_ERR_TRT_ENGINE            = -30,  //!< There was a problem deserializing the inference runtime engine.
  NVCV_ERR_NPP                   = -31,  //!< An error has occurred in the NPP library.
  NVCV_ERR_CONFIG                = -32,  //!< No suitable model exists for the specified parameter configuration.
  NVCV_ERR_TOOSMALL              = -33,  //!< A supplied parameter or buffer is not large enough.
  NVCV_ERR_TOOBIG                = -34,  //!< A supplied parameter is too big.
  NVCV_ERR_WRONGSIZE             = -35,  //!< A supplied parameter is not the expected size.
  NVCV_ERR_OBJECTNOTFOUND        = -36,  //!< The specified object was not found.
  NVCV_ERR_SINGULAR              = -37,  //!< A mathematical singularity has been encountered.
  NVCV_ERR_NOTHINGRENDERED       = -38,  //!< Nothing was rendered in the specified region.
  NVCV_ERR_CONVERGENCE           = -39,  //!< An iteration did not converge satisfactorily.
  NVCV_ERR_EOF                   = -40,  //!< There are no more items in the sequence.
  NVCV_ERR_VERSION_MISMATCH      = -41,  //!< There is a version mismatch between the library and the feature.

  NVCV_ERR_TRITON_NOSERVICE      = -100,  //!< Unable to connect to Triton server.

  NVCV_ERR_GL_BASE               = -200,  //!< OpenGL error codes are starting here.
  NVCV_ERR_GL_INVALIDENUM        = -200,  //!< An invalid enumeration has been supplied to OpenGL.
  NVCV_ERR_GL_INVALIDVALUE       = -201,  //!< An invalid value has been supplied to OpenGL.
  NVCV_ERR_GL_INVALIDOPERATION   = -202,  //!< An invalid operation has been requested from OpenGL.
  NVCV_ERR_GL_STACKOVERFLOW      = -203,  //!< The OpenGL stack is full.
  NVCV_ERR_GL_STACKUNDERFLOW     = -204,  //!< The OpenGL stack is empty.
  NVCV_ERR_GL_MEMORY             = -205,  //!< OpenGL has run out of memory.
  NVCV_ERR_GL_INVALIDFRAMEBUFOP  = -206,  //!< An attempt to access an OpenGL frame buffer that is not complete.
  NVCV_ERR_GL_CONTEXTLOST        = -207,  //!< The OpenGL context has been lost.
  NVCV_ERR_OPENGL                = -208,  //!< An unspecified OpenGL error has occurred.

  NVCV_ERR_DIRECT3D              = -210,  //!< A Direct3D error has occurred.

  NVCV_ERR_CUDA_BASE             = -1000, //!< CUDA errors are offset from this value.
  NVCV_ERR_CUDA_VALUE            = -1001, //!< A CUDA parameter is not within the acceptable range.
  NVCV_ERR_CUDA_MEMORY           = -1002, //!< There is not enough CUDA memory for the requested operation.
  NVCV_ERR_CUDA_PITCH            = -1012, //!< A CUDA pitch is not within the acceptable range.
  NVCV_ERR_CUDA_INIT             = -1003, //!< The CUDA driver and runtime could not be initialized.
  NVCV_ERR_CUDA_LAUNCH           = -1719, //!< The CUDA kernel launch has failed.
  NVCV_ERR_CUDA_KERNEL           = -1209, //!< No suitable kernel image is available for the device.
  NVCV_ERR_CUDA_DRIVER           = -1035, //!< The installed NVIDIA CUDA driver is older than the CUDA runtime library.
  NVCV_ERR_CUDA_UNSUPPORTED      = -1801, //!< The CUDA operation is not supported on the current system or device.
  NVCV_ERR_CUDA_ILLEGAL_ADDRESS  = -1700, //!< CUDA tried to load or store on an invalid memory address.
  NVCV_ERR_CUDA                  = -1999, //!< An otherwise unspecified CUDA error has been reported.

  NVCV_ERR_APP_BASE              = +100,  //!< Application-specific error codes are positive and at least this large.
} NvCV_Status;


//! Get an error string corresponding to the given status code.
//! \param[in]  code  the NvCV status code.
//! \return     the corresponding string.
//! \todo Find a cleaner way to do this, because NvCV_API doesn't work.
#ifdef _WIN32
  __declspec(dllexport) const char* __cdecl
#else
  const char* 
#endif // _WIN32 or linux
NvCV_GetErrorStringFromCode(NvCV_Status code);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __NVCVSTATUS_H__

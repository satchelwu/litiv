
// This file is part of the LITIV framework; visit the original repository at
// https://github.com/plstcharles/litiv for more information.
//
// Copyright 2015 Pierre-Luc St-Charles; pierre-luc.st-charles<at>polymtl.ca
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "litiv/utils/ParallelUtils.hpp"
#include "litiv/utils/OpenCVUtils.hpp"

template<ParallelUtils::eParallelAlgoType eImpl, typename enable=void>
class EdgeDetector_;

struct IEdgeDetector : public cv::Algorithm {

    //! returns the default threshold value used in 'apply'
    virtual double getDefaultThreshold() const = 0;
    //! thresholded edge detection function; the threshold should be between 0 and 1
    virtual void apply_threshold(cv::InputArray oInputImage, cv::OutputArray oEdgeMask, double dThreshold) = 0;
    //! edge detection function which returns a binned confidence edge mask instead of a thresholded/binary edge mask
    virtual void apply(cv::InputArray oInputImage, cv::OutputArray oEdgeMask) = 0;
    //! required for derived class destruction from this interface
    virtual ~IEdgeDetector() {}

protected:
    IEdgeDetector() {}
private:
    IEdgeDetector& operator=(const IEdgeDetector&) = delete;
    IEdgeDetector(const IEdgeDetector&) = delete;
};

template<ParallelUtils::eParallelAlgoType eImpl>
class IEdgeDetector_ :
        public ParallelUtils::ParallelAlgo_<eImpl>,
        public IEdgeDetector {
public:
    //! default impl constructor
    template<ParallelUtils::eParallelAlgoType eImplTemp = eImpl>
    IEdgeDetector_(size_t nROIBorderSize, typename std::enable_if<eImplTemp==ParallelUtils::eNonParallel>::type* /*pUnused*/=0) :
            ParallelUtils::ParallelAlgo_<ParallelUtils::eNonParallel>(),
            m_nROIBorderSize(nROIBorderSize) {}
#if HAVE_GLSL
    //! glsl impl constructor
    template<ParallelUtils::eParallelAlgoType eImplTemp = eImpl>
    IEdgeDetector_(size_t nLevels, size_t nComputeStages, size_t nExtraSSBOs, size_t nExtraACBOs,
                   size_t nExtraImages, size_t nExtraTextures, int nDebugType, bool bUseDisplay,
                   bool bUseTimers, bool bUseIntegralFormat, size_t nROIBorderSize=0,
                   typename std::enable_if<eImplTemp==ParallelUtils::eGLSL>::type* /*pUnused*/=0) :
            ParallelUtils::ParallelAlgo_<ParallelUtils::eGLSL>(nLevels,nComputeStages,nExtraSSBOs,
                                                               nExtraACBOs,nExtraImages,nExtraTextures,
                                                               CV_8UC1,nDebugType,true,bUseDisplay,
                                                               bUseTimers,bUseIntegralFormat),
            m_nROIBorderSize(nROIBorderSize) {}
#endif //HAVE_GLSL
#if HAVE_CUDA
    static_assert(eImpl!=ParallelUtils::eCUDA),"Missing constr impl");
#endif //HAVE_CUDA
#if HAVE_OPENCL
    static_assert(eImpl!=ParallelUtils::eOpenCL),"Missing constr impl");
#endif //HAVE_OPENCL

    //! required for derived class destruction from this interface
    virtual ~IEdgeDetector_() {}

protected:
    //! ROI border size to be ignored, useful for descriptor-based methods
    size_t m_nROIBorderSize;

private:
    IEdgeDetector_& operator=(const IEdgeDetector_&) = delete;
    IEdgeDetector_(const IEdgeDetector_&) = delete;
public:
    // #### for debug purposes only ####
    cv::DisplayHelperPtr m_pDisplayHelper;
};

#if HAVE_GLSL
template<ParallelUtils::eParallelAlgoType eImpl>
class EdgeDetector_<eImpl, typename std::enable_if<eImpl==ParallelUtils::eGLSL>::type> :
        public IEdgeDetector_<ParallelUtils::eGLSL> {
public:
    //! glsl impl constructor
    EdgeDetector_(size_t nLevels, size_t nComputeStages, size_t nExtraSSBOs, size_t nExtraACBOs,
                  size_t nExtraImages, size_t nExtraTextures, int nDebugType, bool bUseDisplay,
                  bool bUseTimers, bool bUseIntegralFormat, size_t nROIBorderSize=0);

    //! returns a copy of the latest edge mask
    void getLatestEdgeMask(cv::OutputArray _oLastEdgeMask);
    //! edge detection function (asynchronous version, glsl interface); the threshold should be between 0 and 1, or -1 for the confidence mask version
    void apply_async_glimpl(cv::InputArray _oNextImage, bool bRebindAll, double dThreshold);
    //! edge detection function (asynchronous version); the threshold should be between 0 and 1, or -1 for the confidence mask version
    void apply_async(cv::InputArray oNextImage, double dThreshold);
    //! edge detection function (asynchronous version); the threshold should be between 0 and 1, or -1 for the confidence mask version
    void apply_async(cv::InputArray oNextImage, cv::OutputArray oEdgeMask, double dThreshold);
    //! overloads 'apply_threshold' from EdgeDetector and redirects it to apply_async
    virtual void apply_threshold(cv::InputArray oNextImage, cv::OutputArray oLastEdgeMask, double dThreshold);
    //! overloads 'apply' from EdgeDetector and redirects it to apply_async
    virtual void apply(cv::InputArray oNextImage, cv::OutputArray oEdgeMask);

protected:
    //! used to pass 'apply' threshold parameter to overloaded dispatch call, if needed
    double m_dCurrThreshold;
};
typedef EdgeDetector_<ParallelUtils::eGLSL> EdgeDetector_GLSL;
#endif //!HAVE_GLSL

#if HAVE_CUDA
template<ParallelUtils::eParallelAlgoType eImpl>
class EdgeDetector_<eImpl, typename std::enable_if<eImpl==ParallelUtils::eCUDA>::type> :
        public IEdgeDetector_<ParallelUtils::eCUDA> {
public:
    static_assert(false,"Missing CUDA impl");
};
typedef EdgeDetector_<ParallelUtils::eCUDA> EdgeDetector_CUDA;
#endif //HAVE_CUDA

#if HAVE_OPENCL
template<ParallelUtils::eParallelAlgoType eImpl>
class EdgeDetector_<eImpl, typename std::enable_if<eImpl==ParallelUtils::eOpenCL>::type> :
        public IEdgeDetector_<ParallelUtils::eOpenCL> {
public:
    static_assert(false,"Missing OpenCL impl");
};
typedef EdgeDetector_<ParallelUtils::eOpenCL> EdgeDetector_OpenCL;
#endif //HAVE_OPENCL

template<ParallelUtils::eParallelAlgoType eImpl>
class EdgeDetector_<eImpl, typename std::enable_if<eImpl==ParallelUtils::eNonParallel>::type> :
        public IEdgeDetector_<ParallelUtils::eNonParallel> {
public:
    //! default impl constructor
    EdgeDetector_(size_t nROIBorderSize);
};
typedef EdgeDetector_<ParallelUtils::eNonParallel> EdgeDetector;

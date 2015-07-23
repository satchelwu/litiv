#include "litiv/utils/DatasetUtils.hpp"
#include "litiv/utils/DatasetEvalUtils.hpp"

#define PRECACHE_CONSOLE_DEBUG             0
#define PRECACHE_REQUEST_TIMEOUT_MS        1
#define PRECACHE_QUERY_TIMEOUT_MS          10
#define PRECACHE_MAX_CACHE_SIZE_GB         6L
#define PRECACHE_MAX_CACHE_SIZE            (((PRECACHE_MAX_CACHE_SIZE_GB*1024)*1024)*1024)
#if (!(defined(_M_X64) || defined(__amd64__)) && PRECACHE_MAX_CACHE_SIZE_GB>2)
#error "Cache max size exceeds system limit (x86)."
#endif //(!(defined(_M_X64) || defined(__amd64__)) && PRECACHE_MAX_CACHE_SIZE_GB>2)

void DatasetUtils::WriteOnImage(cv::Mat& oImg, const std::string& sText, const cv::Scalar& vColor, bool bBottom) {
    cv::putText(oImg,sText,cv::Point(4,bBottom?(oImg.rows-15):15),cv::FONT_HERSHEY_PLAIN,1.2,vColor,2,cv::LINE_AA);
}

void DatasetUtils::ValidateKeyPoints(const cv::Mat& oROI, std::vector<cv::KeyPoint>& voKPs) {
    std::vector<cv::KeyPoint> voNewKPs;
    for(size_t k=0; k<voKPs.size(); ++k) {
        if( voKPs[k].pt.x>=0 && voKPs[k].pt.x<oROI.cols &&
            voKPs[k].pt.y>=0 && voKPs[k].pt.y<oROI.rows &&
            oROI.at<uchar>(voKPs[k].pt)>0)
            voNewKPs.push_back(voKPs[k]);
    }
    voKPs = voNewKPs;
}

DatasetUtils::ImagePrecacher::ImagePrecacher(ImageQueryByIndexFunc pCallback) {
    CV_Assert(pCallback);
    m_pCallback = pCallback;
    m_bIsPrecaching = false;
    m_nLastReqIdx = size_t(-1);
}

DatasetUtils::ImagePrecacher::~ImagePrecacher() {
    StopPrecaching();
}

const cv::Mat& DatasetUtils::ImagePrecacher::GetImageFromIndex(size_t nIdx) {
    if(!m_bIsPrecaching)
        return GetImageFromIndex_internal(nIdx);
    CV_Assert(nIdx<m_nTotImageCount);
    std::unique_lock<std::mutex> sync_lock(m_oSyncMutex);
    m_nReqIdx = nIdx;
    std::cv_status res;
    do {
        m_oReqCondVar.notify_one();
        res = m_oSyncCondVar.wait_for(sync_lock,std::chrono::milliseconds(PRECACHE_REQUEST_TIMEOUT_MS));
#if PRECACHE_CONSOLE_DEBUG
        if(res==std::cv_status::timeout)
            std::cout << " # retrying request..." << std::endl;
#endif //PRECACHE_CONSOLE_DEBUG
    } while(res==std::cv_status::timeout);
    return m_oReqImage;
}

bool DatasetUtils::ImagePrecacher::StartPrecaching(size_t nTotImageCount, size_t nSuggestedBufferSize) {
    static_assert(PRECACHE_REQUEST_TIMEOUT_MS>0,"Precache request timeout must be a positive value");
    static_assert(PRECACHE_QUERY_TIMEOUT_MS>0,"Precache query timeout must be a positive value");
    static_assert(PRECACHE_MAX_CACHE_SIZE>=0,"Precache size must be a non-negative value");
    CV_Assert(nTotImageCount);
    m_nTotImageCount = nTotImageCount;
    if(m_bIsPrecaching)
        StopPrecaching();
    if(nSuggestedBufferSize>0) {
        m_bIsPrecaching = true;
        m_nBufferSize = (nSuggestedBufferSize>PRECACHE_MAX_CACHE_SIZE)?(PRECACHE_MAX_CACHE_SIZE):nSuggestedBufferSize;
        m_qoCache.clear();
        m_vcBuffer.resize(m_nBufferSize);
        m_nNextExpectedReqIdx = 0;
        m_nNextPrecacheIdx = 0;
        m_nReqIdx = m_nLastReqIdx = size_t(-1);
        m_hPrecacher = std::thread(&DatasetUtils::ImagePrecacher::Precache,this);
    }
    return m_bIsPrecaching;
}

void DatasetUtils::ImagePrecacher::StopPrecaching() {
    if(m_bIsPrecaching) {
        m_bIsPrecaching = false;
        m_hPrecacher.join();
    }
}

void DatasetUtils::ImagePrecacher::Precache() {
    std::unique_lock<std::mutex> sync_lock(m_oSyncMutex);
#if PRECACHE_CONSOLE_DEBUG
    std::cout << " @ initializing precaching with buffer size = " << (m_nBufferSize/1024)/1024 << " mb" << std::endl;
#endif //PRECACHE_CONSOLE_DEBUG
    m_nFirstBufferIdx = m_nNextBufferIdx = 0;
    while(m_nNextPrecacheIdx<m_nTotImageCount) {
        const cv::Mat& oNextImage = GetImageFromIndex_internal(m_nNextPrecacheIdx);
        const size_t nNextImageSize = oNextImage.total()*oNextImage.elemSize();
        if(m_nNextBufferIdx+nNextImageSize<m_nBufferSize) {
            cv::Mat oNextImage_cache(oNextImage.size(),oNextImage.type(),m_vcBuffer.data()+m_nNextBufferIdx);
            oNextImage.copyTo(oNextImage_cache);
            m_qoCache.push_back(oNextImage_cache);
            m_nNextBufferIdx += nNextImageSize;
            ++m_nNextPrecacheIdx;
        }
        else break;
    }
    while(m_bIsPrecaching) {
        if(m_oReqCondVar.wait_for(sync_lock,std::chrono::milliseconds(m_nNextPrecacheIdx==m_nTotImageCount?PRECACHE_QUERY_TIMEOUT_MS*32:PRECACHE_QUERY_TIMEOUT_MS))!=std::cv_status::timeout) {
            if(m_nReqIdx!=m_nNextExpectedReqIdx-1) {
                if(!m_qoCache.empty()) {
                    if(m_nReqIdx<m_nNextPrecacheIdx && m_nReqIdx>=m_nNextExpectedReqIdx) {
//#if PRECACHE_CONSOLE_DEBUG
//                        std::cout << " -- popping " << m_nReqIdx-m_nNextExpectedReqIdx+1 << " image(s) from cache" << std::endl;
//#endif //PRECACHE_CONSOLE_DEBUG
                        while(m_nReqIdx-m_nNextExpectedReqIdx+1>0) {
                            m_oReqImage = m_qoCache.front();
                            m_nFirstBufferIdx = (size_t)(m_oReqImage.data-m_vcBuffer.data());
                            m_qoCache.pop_front();
                            ++m_nNextExpectedReqIdx;
                        }
                    }
                    else {
#if PRECACHE_CONSOLE_DEBUG
                        std::cout << " -- out-of-order request, destroying cache" << std::endl;
#endif //PRECACHE_CONSOLE_DEBUG
                        m_qoCache.clear();
                        m_oReqImage = GetImageFromIndex_internal(m_nReqIdx);
                        m_nFirstBufferIdx = m_nNextBufferIdx = size_t(-1);
                        m_nNextExpectedReqIdx = m_nNextPrecacheIdx = m_nReqIdx+1;
                    }
                }
                else {
#if PRECACHE_CONSOLE_DEBUG
                    std::cout << " @ answering request manually, precaching is falling behind" << std::endl;
#endif //PRECACHE_CONSOLE_DEBUG
                    m_oReqImage = GetImageFromIndex_internal(m_nReqIdx);
                    m_nFirstBufferIdx = m_nNextBufferIdx = size_t(-1);
                    m_nNextExpectedReqIdx = m_nNextPrecacheIdx = m_nReqIdx+1;
                }
            }
#if PRECACHE_CONSOLE_DEBUG
            else
                std::cout << " @ answering request using last image" << std::endl;
#endif //PRECACHE_CONSOLE_DEBUG
            m_oSyncCondVar.notify_one();
        }
        else {
            size_t nUsedBufferSize = m_nFirstBufferIdx==size_t(-1)?0:(m_nFirstBufferIdx<m_nNextBufferIdx?m_nNextBufferIdx-m_nFirstBufferIdx:m_nBufferSize-m_nFirstBufferIdx+m_nNextBufferIdx);
            if(nUsedBufferSize<m_nBufferSize/4 && m_nNextPrecacheIdx<m_nTotImageCount) {
#if PRECACHE_CONSOLE_DEBUG
                std::cout << " @ filling precache buffer... (current size = " << (nUsedBufferSize/1024)/1024 << " mb)" << std::endl;
#endif //PRECACHE_CONSOLE_DEBUG
                size_t nFillCount = 0;
                while(nUsedBufferSize<m_nBufferSize && m_nNextPrecacheIdx<m_nTotImageCount && nFillCount<10) {
                    const cv::Mat& oNextImage = GetImageFromIndex_internal(m_nNextPrecacheIdx);
                    const size_t nNextImageSize = (oNextImage.total()*oNextImage.elemSize());
                    if(m_nFirstBufferIdx<=m_nNextBufferIdx) {
                        if(m_nNextBufferIdx==size_t(-1) || (m_nNextBufferIdx+nNextImageSize>=m_nBufferSize)) {
                            if((m_nFirstBufferIdx!=size_t(-1) && nNextImageSize>=m_nFirstBufferIdx) || nNextImageSize>=m_nBufferSize)
                                break;
                            cv::Mat oNextImage_cache(oNextImage.size(),oNextImage.type(),m_vcBuffer.data());
                            oNextImage.copyTo(oNextImage_cache);
                            m_qoCache.push_back(oNextImage_cache);
                            m_nNextBufferIdx = nNextImageSize;
                            if(m_nFirstBufferIdx==size_t(-1))
                                m_nFirstBufferIdx = 0;
                        }
                        else { // m_nNextBufferIdx+nNextImageSize<m_nBufferSize
                            cv::Mat oNextImage_cache(oNextImage.size(),oNextImage.type(),m_vcBuffer.data()+m_nNextBufferIdx);
                            oNextImage.copyTo(oNextImage_cache);
                            m_qoCache.push_back(oNextImage_cache);
                            m_nNextBufferIdx += nNextImageSize;
                        }
                    }
                    else if(m_nNextBufferIdx+nNextImageSize<m_nFirstBufferIdx) {
                        cv::Mat oNextImage_cache(oNextImage.size(),oNextImage.type(),m_vcBuffer.data()+m_nNextBufferIdx);
                        oNextImage.copyTo(oNextImage_cache);
                        m_qoCache.push_back(oNextImage_cache);
                        m_nNextBufferIdx += nNextImageSize;
                    }
                    else // m_nNextBufferIdx+nNextImageSize>=m_nFirstBufferIdx
                        break;
                    nUsedBufferSize += nNextImageSize;
                    ++m_nNextPrecacheIdx;
                }
            }
        }
    }
}

const cv::Mat& DatasetUtils::ImagePrecacher::GetImageFromIndex_internal(size_t nIdx) {
    if(m_nLastReqIdx!=nIdx) {
        m_oLastReqImage = m_pCallback(nIdx);
        m_nLastReqIdx = nIdx;
    }
    return m_oLastReqImage;
}

DatasetUtils::WorkBatch::WorkBatch(const std::string& sName, const std::string& sPath, bool bHasGT, bool bForceGrayscale, bool bUse4chAlign)
    :    m_sName(sName)
        ,m_sPath(sPath)
        ,m_bHasGroundTruth(bHasGT)
        ,m_bForcingGrayscale(bForceGrayscale)
        ,m_bUsing4chAlignment(bUse4chAlign)
        ,m_nIMReadInputFlags(bForceGrayscale?cv::IMREAD_GRAYSCALE:cv::IMREAD_COLOR)
        ,m_oInputPrecacher(std::bind(&DatasetUtils::WorkBatch::GetInputFromIndex_internal,this,std::placeholders::_1))
        ,m_oGTPrecacher(std::bind(&DatasetUtils::WorkBatch::GetGTFromIndex_internal,this,std::placeholders::_1)) {}

bool DatasetUtils::WorkBatch::StartPrecaching(size_t nSuggestedBufferSize) {
    return m_oInputPrecacher.StartPrecaching(GetTotalImageCount(),nSuggestedBufferSize) && (!m_bHasGroundTruth || m_oGTPrecacher.StartPrecaching(GetTotalImageCount(),nSuggestedBufferSize));
}

void DatasetUtils::WorkBatch::StopPrecaching() {
    m_oInputPrecacher.StopPrecaching();
    m_oGTPrecacher.StopPrecaching();
}

const cv::Mat& DatasetUtils::WorkBatch::GetInputFromIndex_internal(size_t nIdx) {
    CV_Assert(nIdx<GetTotalImageCount());
    m_oLatestInputImage = GetInputFromIndex_external(nIdx);
    return m_oLatestInputImage;
}

const cv::Mat& DatasetUtils::WorkBatch::GetGTFromIndex_internal(size_t nIdx) {
    CV_Assert(nIdx<GetTotalImageCount());
    m_oLatestGTMask = GetGTFromIndex_external(nIdx);
    return m_oLatestGTMask;
}

DatasetUtils::Segm::BasicMetrics::BasicMetrics()
    :   nTP(0),nTN(0),nFP(0),nFN(0),nSE(0),dTimeElapsed_sec(0) {}

DatasetUtils::Segm::BasicMetrics DatasetUtils::Segm::BasicMetrics::operator+(const BasicMetrics& m) const {
    BasicMetrics res(m);
    res.nTP += this->nTP;
    res.nTN += this->nTN;
    res.nFP += this->nFP;
    res.nFN += this->nFN;
    res.nSE += this->nSE;
    res.dTimeElapsed_sec += this->dTimeElapsed_sec;
    return res;
}

DatasetUtils::Segm::BasicMetrics& DatasetUtils::Segm::BasicMetrics::operator+=(const BasicMetrics& m) {
    this->nTP += m.nTP;
    this->nTN += m.nTN;
    this->nFP += m.nFP;
    this->nFN += m.nFN;
    this->nSE += m.nSE;
    this->dTimeElapsed_sec += m.dTimeElapsed_sec;
    return *this;
}

DatasetUtils::Segm::Metrics::Metrics(const DatasetUtils::Segm::BasicMetrics& m)
    :    dRecall(CalcRecall(m))
        ,dSpecificity(CalcSpecificity(m))
        ,dFPR(CalcFalsePositiveRate(m))
        ,dFNR(CalcFalseNegativeRate(m))
        ,dPBC(CalcPercentBadClassifs(m))
        ,dPrecision(CalcPrecision(m))
        ,dFMeasure(CalcFMeasure(m))
        ,dMCC(CalcMatthewsCorrCoeff(m))
        ,dTimeElapsed_sec(m.dTimeElapsed_sec)
        ,nWeight(1) {}

DatasetUtils::Segm::Metrics DatasetUtils::Segm::Metrics::operator+(const DatasetUtils::Segm::BasicMetrics& m) const {
    Metrics tmp(m);
    return (*this)+tmp;
}

DatasetUtils::Segm::Metrics& DatasetUtils::Segm::Metrics::operator+=(const DatasetUtils::Segm::BasicMetrics& m) {
    Metrics tmp(m);
    (*this) += tmp;
    return *this;
}

DatasetUtils::Segm::Metrics DatasetUtils::Segm::Metrics::operator+(const DatasetUtils::Segm::Metrics& m) const {
    Metrics res(m);
    const size_t nTotWeight = this->nWeight+res.nWeight;
    res.dRecall = (res.dRecall*res.nWeight + this->dRecall*this->nWeight)/nTotWeight;
    res.dSpecificity = (res.dSpecificity*res.nWeight + this->dSpecificity*this->nWeight)/nTotWeight;
    res.dFPR = (res.dFPR*res.nWeight + this->dFPR*this->nWeight)/nTotWeight;
    res.dFNR = (res.dFNR*res.nWeight + this->dFNR*this->nWeight)/nTotWeight;
    res.dPBC = (res.dPBC*res.nWeight + this->dPBC*this->nWeight)/nTotWeight;
    res.dPrecision = (res.dPrecision*res.nWeight + this->dPrecision*this->nWeight)/nTotWeight;
    res.dFMeasure = (res.dFMeasure*res.nWeight + this->dFMeasure*this->nWeight)/nTotWeight;
    res.dMCC = (res.dMCC*res.nWeight + this->dMCC*this->nWeight)/nTotWeight;
    res.dTimeElapsed_sec += this->dTimeElapsed_sec;
    res.nWeight = nTotWeight;
    return res;
}

DatasetUtils::Segm::Metrics& DatasetUtils::Segm::Metrics::operator+=(const DatasetUtils::Segm::Metrics& m) {
    const size_t nTotWeight = this->nWeight+m.nWeight;
    this->dRecall = (m.dRecall*m.nWeight + this->dRecall*this->nWeight)/nTotWeight;
    this->dSpecificity = (m.dSpecificity*m.nWeight + this->dSpecificity*this->nWeight)/nTotWeight;
    this->dFPR = (m.dFPR*m.nWeight + this->dFPR*this->nWeight)/nTotWeight;
    this->dFNR = (m.dFNR*m.nWeight + this->dFNR*this->nWeight)/nTotWeight;
    this->dPBC = (m.dPBC*m.nWeight + this->dPBC*this->nWeight)/nTotWeight;
    this->dPrecision = (m.dPrecision*m.nWeight + this->dPrecision*this->nWeight)/nTotWeight;
    this->dFMeasure = (m.dFMeasure*m.nWeight + this->dFMeasure*this->nWeight)/nTotWeight;
    this->dMCC = (m.dMCC*m.nWeight + this->dMCC*this->nWeight)/nTotWeight;
    this->dTimeElapsed_sec += m.dTimeElapsed_sec;
    this->nWeight = nTotWeight;
    return *this;
}

double DatasetUtils::Segm::Metrics::CalcFMeasure(const DatasetUtils::Segm::BasicMetrics& m) {
    const double dRecall = CalcRecall(m);
    const double dPrecision = CalcPrecision(m);
    return (2.0*(dRecall*dPrecision)/(dRecall+dPrecision));
}
double DatasetUtils::Segm::Metrics::CalcRecall(const DatasetUtils::Segm::BasicMetrics& m) {return ((double)m.nTP/(m.nTP+m.nFN));}
double DatasetUtils::Segm::Metrics::CalcPrecision(const DatasetUtils::Segm::BasicMetrics& m) {return ((double)m.nTP/(m.nTP+m.nFP));}
double DatasetUtils::Segm::Metrics::CalcSpecificity(const DatasetUtils::Segm::BasicMetrics& m) {return ((double)m.nTN/(m.nTN+m.nFP));}
double DatasetUtils::Segm::Metrics::CalcFalsePositiveRate(const DatasetUtils::Segm::BasicMetrics& m) {return ((double)m.nFP/(m.nFP+m.nTN));}
double DatasetUtils::Segm::Metrics::CalcFalseNegativeRate(const DatasetUtils::Segm::BasicMetrics& m) {return ((double)m.nFN/(m.nTP+m.nFN));}
double DatasetUtils::Segm::Metrics::CalcPercentBadClassifs(const DatasetUtils::Segm::BasicMetrics& m) {return (100.0*(m.nFN+m.nFP)/(m.nTP+m.nFP+m.nFN+m.nTN));}
double DatasetUtils::Segm::Metrics::CalcMatthewsCorrCoeff(const DatasetUtils::Segm::BasicMetrics& m) {return ((((double)m.nTP*m.nTN)-(m.nFP*m.nFN))/sqrt(((double)m.nTP+m.nFP)*(m.nTP+m.nFN)*(m.nTN+m.nFP)*(m.nTN+m.nFN)));}

DatasetUtils::Segm::Video::DatasetInfo DatasetUtils::Segm::Video::GetDatasetInfo(const DatasetUtils::Segm::Video::eDatasetList eDatasetID, const std::string& sDatasetRootDirPath, const std::string& sResultsDirPath, bool bLoad4Channels) {
    if(eDatasetID==eDataset_CDnet2012)
        return DatasetInfo {
            eDatasetID,
            sDatasetRootDirPath+"/CDNet/dataset/",
            sDatasetRootDirPath+"/CDNet/"+sResultsDirPath+"/",
            "bin",
            ".png",
            {"baseline","cameraJitter","dynamicBackground","intermittentObjectMotion","shadow","thermal"},
            {"thermal"},
            {},
            1,
            std::shared_ptr<SegmEvaluator>(new CDnetEvaluator),
            bLoad4Channels
        };
    else if(eDatasetID==eDataset_CDnet2014)
        return DatasetInfo {
            eDatasetID,
            sDatasetRootDirPath+"/CDNet2014/dataset/",
            sDatasetRootDirPath+"/CDNet2014/"+sResultsDirPath+"/",
            "bin",
            ".png",
            {"baseline_highway"},//{"shadow_cubicle"},//{"dynamicBackground_fall"},//{"badWeather","baseline","cameraJitter","dynamicBackground","intermittentObjectMotion","lowFramerate","nightVideos","PTZ","shadow","thermal","turbulence"},
            {"thermal","turbulence"},//{"baseline_highway"},//
            {},
            1,
            nullptr,//std::shared_ptr<SegmEvaluator>(new CDnetEvaluator),
            bLoad4Channels
        };
    else if(eDatasetID==eDataset_Wallflower)
        return DatasetInfo {
            eDatasetID,
            sDatasetRootDirPath+"/Wallflower/dataset/",
            sDatasetRootDirPath+"/Wallflower/"+sResultsDirPath+"/",
            "bin",
            ".png",
            {"global"},
            {},
            {},
            0,
            nullptr,//std::shared_ptr<SegmEvaluator>(new BinarySegmEvaluator),
            bLoad4Channels
        };
    else if(eDatasetID==eDataset_PETS2001_D3TC1)
        return DatasetInfo {
            eDatasetID,
            sDatasetRootDirPath+"/PETS2001/DATASET3/",
            sDatasetRootDirPath+"/PETS2001/DATASET3/"+sResultsDirPath+"/",
            "bin",
            ".png",
            {"TESTING"},
            {},
            {},
            0,
            nullptr,//std::shared_ptr<SegmEvaluator>(new BinarySegmEvaluator),
            bLoad4Channels
        };
    else if(eDatasetID==eDataset_LITIV2012)
        return DatasetInfo {
            eDatasetID,
            sDatasetRootDirPath+"/litiv/litiv2012_dataset/",
            sDatasetRootDirPath+"/litiv/litiv2012_dataset/"+sResultsDirPath+"/",
            "bin",
            ".png",
            {"SEQUENCE1","SEQUENCE2","SEQUENCE3","SEQUENCE4","SEQUENCE5","SEQUENCE6","SEQUENCE7","SEQUENCE8","SEQUENCE9"},//{"vid1","vid2/cut1","vid2/cut2","vid3"},
            {"THERMAL"},
            {},//{"1Person","2Person","3Person","4Person","5Person"},
            0,
            nullptr,
            bLoad4Channels
        };
    else if(eDatasetID==eDataset_GenericTest)
        // @@@@@ remove from this func, set only in main where/when needed?
        return DatasetInfo {
            eDatasetID,
            sDatasetRootDirPath+"/avitest/",                        // HARDCODED
            sDatasetRootDirPath+"/avitest/"+sResultsDirPath+"/",    // HARDCODED
            "",                                                     // HARDCODED
            ".png",                                                 // HARDCODED
            {"inf6803_tp1"},                                        // HARDCODED
            {},                                                     // HARDCODED
            {},                                                     // HARDCODED
            0,                                                      // HARDCODED
            nullptr,
            bLoad4Channels
        };
    else
        throw std::runtime_error(std::string("Unknown dataset type, cannot use predefined info struct"));
}

DatasetUtils::Segm::Video::CategoryInfo::CategoryInfo( const std::string& sName, const std::string& sDirectoryPath, const DatasetUtils::Segm::Video::DatasetInfo& oInfo)
    :    WorkBatch(sName,sDirectoryPath,oInfo.pEvaluator!=nullptr,PlatformUtils::string_contains_token(sName,oInfo.vsGrayscaleNameTokens),oInfo.bLoad4Channels)
        ,m_eDatasetID(oInfo.eID)
        ,m_dExpectedLoad(0)
        ,m_nTotFrameCount(0) {
    std::cout<<"\tParsing dir '"<<sDirectoryPath<<"' for category '"<<m_sName<<"'; ";
    std::vector<std::string> vsSequencePaths;
    if(m_eDatasetID == eDataset_CDnet2012 ||
       m_eDatasetID == eDataset_CDnet2014 ||
       m_eDatasetID == eDataset_Wallflower ||
       m_eDatasetID == eDataset_PETS2001_D3TC1) {
        // all subdirs are considered sequence directories
        PlatformUtils::GetSubDirsFromDir(sDirectoryPath,vsSequencePaths);
        std::cout<<vsSequencePaths.size()<<" potential sequence(s)"<<std::endl;
    }
    else if(m_eDatasetID == eDataset_LITIV2012) {
        // all subdirs should contain individual video tracks in separate modalities
        PlatformUtils::GetSubDirsFromDir(sDirectoryPath,vsSequencePaths);
        std::cout<<vsSequencePaths.size()<<" potential track(s)"<<std::endl;
    }
    else if(m_eDatasetID == eDataset_GenericTest) {
        // all files are considered sequences
        PlatformUtils::GetFilesFromDir(sDirectoryPath,vsSequencePaths);
        std::cout<<vsSequencePaths.size()<<" potential sequence(s)"<<std::endl;
    }
    else
        throw std::runtime_error(std::string("Unknown dataset type, cannot use any known parsing strategy"));
    if(!PlatformUtils::string_contains_token(sName,oInfo.vsSkippedNameTokens)) {
        for(auto oSeqPathIter=vsSequencePaths.begin(); oSeqPathIter!=vsSequencePaths.end(); ++oSeqPathIter) {
            const size_t idx = oSeqPathIter->find_last_of("/\\");
            const std::string sSeqName = idx==std::string::npos?*oSeqPathIter:oSeqPathIter->substr(idx+1);
            if(!PlatformUtils::string_contains_token(sSeqName,oInfo.vsSkippedNameTokens)) {
                m_vpSequences.push_back(std::make_shared<SequenceInfo>(sSeqName,m_sName,*oSeqPathIter,oInfo));
                m_dExpectedLoad += m_vpSequences.back()->GetExpectedLoad();
                m_nTotFrameCount += m_vpSequences.back()->GetTotalImageCount();
            }
        }
    }
}

cv::Mat DatasetUtils::Segm::Video::CategoryInfo::GetInputFromIndex_external(size_t nFrameIdx) {
    size_t nCumulFrameIdx = 0;
    size_t nSeqIdx = 0;
    do {
        nCumulFrameIdx += m_vpSequences[nSeqIdx++]->GetTotalImageCount();
    } while(nCumulFrameIdx<nFrameIdx);
    return m_vpSequences[nSeqIdx-1]->GetInputFromIndex_external(nFrameIdx-(nCumulFrameIdx-m_vpSequences[nSeqIdx-1]->GetTotalImageCount()));
}

cv::Mat DatasetUtils::Segm::Video::CategoryInfo::GetGTFromIndex_external(size_t nFrameIdx) {
    size_t nCumulFrameIdx = 0;
    size_t nSeqIdx = 0;
    do {
        nCumulFrameIdx += m_vpSequences[nSeqIdx++]->GetTotalImageCount();
    } while(nCumulFrameIdx<nFrameIdx);
    return m_vpSequences[nSeqIdx-1]->GetGTFromIndex_external(nFrameIdx-(nCumulFrameIdx-m_vpSequences[nSeqIdx-1]->GetTotalImageCount()));
}

DatasetUtils::Segm::Video::SequenceInfo::SequenceInfo(const std::string& sName, const std::string& sParentName, const std::string& sPath, const DatasetUtils::Segm::Video::DatasetInfo& oInfo)
    :    WorkBatch(sName,sPath,oInfo.pEvaluator!=nullptr,PlatformUtils::string_contains_token(sName,oInfo.vsGrayscaleNameTokens)||PlatformUtils::string_contains_token(sParentName,oInfo.vsGrayscaleNameTokens),oInfo.bLoad4Channels)
        ,m_eDatasetID(oInfo.eID)
        ,m_sParentName(sParentName)
        ,m_dExpectedLoad(0)
        ,m_nTotFrameCount(0)
        ,m_nNextExpectedVideoReaderFrameIdx(0) {
    if(m_eDatasetID==eDataset_CDnet2012 || m_eDatasetID==eDataset_CDnet2014) {
        std::vector<std::string> vsSubDirs;
        PlatformUtils::GetSubDirsFromDir(m_sPath,vsSubDirs);
        auto gtDir = std::find(vsSubDirs.begin(),vsSubDirs.end(),m_sPath+"/groundtruth");
        auto inputDir = std::find(vsSubDirs.begin(),vsSubDirs.end(),m_sPath+"/input");
        if(gtDir==vsSubDirs.end() || inputDir==vsSubDirs.end())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess the required groundtruth and input directories");
        PlatformUtils::GetFilesFromDir(*inputDir,m_vsInputFramePaths);
        PlatformUtils::GetFilesFromDir(*gtDir,m_vsGTFramePaths);
        if(m_vsGTFramePaths.size()!=m_vsInputFramePaths.size())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess same amount of GT & input frames");
        m_oROI = cv::imread(m_sPath+"/ROI.bmp",cv::IMREAD_GRAYSCALE);
        if(m_oROI.empty())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess a ROI.bmp file");
        m_oROI = m_oROI>0;
        m_oSize = m_oROI.size();
        m_nTotFrameCount = m_vsInputFramePaths.size();
        CV_Assert(m_nTotFrameCount>0);
        m_dExpectedLoad = (double)cv::countNonZero(m_oROI)*m_nTotFrameCount*(int(!m_bForcingGrayscale)+1);
        // note: in this case, no need to use m_vnTestGTIndexes since all # of gt frames == # of test frames (but we assume the frames returned by 'GetFilesFromDir' are ordered correctly...)
    }
    else if(m_eDatasetID==eDataset_Wallflower) {
        std::vector<std::string> vsImgPaths;
        PlatformUtils::GetFilesFromDir(m_sPath,vsImgPaths);
        bool bFoundScript=false, bFoundGTFile=false;
        const std::string sGTFilePrefix("hand_segmented_");
        const size_t nInputFileNbDecimals = 5;
        const std::string sInputFileSuffix(".bmp");
        for(auto iter=vsImgPaths.begin(); iter!=vsImgPaths.end(); ++iter) {
            if(*iter==m_sPath+"/script.txt")
                bFoundScript = true;
            else if(iter->find(sGTFilePrefix)!=std::string::npos) {
                m_mTestGTIndexes.insert(std::pair<size_t,size_t>(atoi(iter->substr(iter->find(sGTFilePrefix)+sGTFilePrefix.size(),nInputFileNbDecimals).c_str()),m_vsGTFramePaths.size()));
                m_vsGTFramePaths.push_back(*iter);
                bFoundGTFile = true;
            }
            else {
                if(iter->find(sInputFileSuffix)!=iter->size()-sInputFileSuffix.size())
                    throw std::runtime_error(std::string("Sequence directory at ") + m_sPath + " contained an unknown file ('" + *iter + "')");
                m_vsInputFramePaths.push_back(*iter);
            }
        }
        if(!bFoundGTFile || !bFoundScript || m_vsInputFramePaths.empty() || m_vsGTFramePaths.size()!=1)
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess the required groundtruth and input files");
        cv::Mat oTempImg = cv::imread(m_vsGTFramePaths[0]);
        if(oTempImg.empty())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess a valid GT file");
        m_oROI = cv::Mat(oTempImg.size(),CV_8UC1,cv::Scalar_<uchar>(255));
        m_oSize = oTempImg.size();
        m_nTotFrameCount = m_vsInputFramePaths.size();
        CV_Assert(m_nTotFrameCount>0);
        m_dExpectedLoad = (double)m_oSize.height*m_oSize.width*m_nTotFrameCount*(int(!m_bForcingGrayscale)+1);
    }
    else if(m_eDatasetID==eDataset_PETS2001_D3TC1) {
        std::vector<std::string> vsVideoSeqPaths;
        PlatformUtils::GetFilesFromDir(m_sPath,vsVideoSeqPaths);
        if(vsVideoSeqPaths.size()!=1)
            throw std::runtime_error(std::string("Bad subdirectory ('")+m_sPath+std::string("') for PETS2001 parsing (should contain only one video sequence file)"));
        std::vector<std::string> vsGTSubdirPaths;
        PlatformUtils::GetSubDirsFromDir(m_sPath,vsGTSubdirPaths);
        if(vsGTSubdirPaths.size()!=1)
            throw std::runtime_error(std::string("Bad subdirectory ('")+m_sPath+std::string("') for PETS2001 parsing (should contain only one GT subdir)"));
        m_voVideoReader.open(vsVideoSeqPaths[0]);
        if(!m_voVideoReader.isOpened())
            throw std::runtime_error(std::string("Bad video file ('")+vsVideoSeqPaths[0]+std::string("'), could not be opened"));
        PlatformUtils::GetFilesFromDir(vsGTSubdirPaths[0],m_vsGTFramePaths);
        if(m_vsGTFramePaths.empty())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess any valid GT frames");
        const std::string sGTFilePrefix("image_");
        const size_t nInputFileNbDecimals = 4;
        for(auto iter=m_vsGTFramePaths.begin(); iter!=m_vsGTFramePaths.end(); ++iter)
            m_mTestGTIndexes.insert(std::pair<size_t,size_t>((size_t)atoi(iter->substr(iter->find(sGTFilePrefix)+sGTFilePrefix.size(),nInputFileNbDecimals).c_str()),iter-m_vsGTFramePaths.begin()));
        cv::Mat oTempImg = cv::imread(m_vsGTFramePaths[0]);
        if(oTempImg.empty())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess valid GT file(s)");
        m_oROI = cv::Mat(oTempImg.size(),CV_8UC1,cv::Scalar_<uchar>(255));
        m_oSize = oTempImg.size();
        m_nNextExpectedVideoReaderFrameIdx = 0;
        m_nTotFrameCount = (size_t)m_voVideoReader.get(cv::CAP_PROP_FRAME_COUNT);
        CV_Assert(m_nTotFrameCount>0);
        m_dExpectedLoad = (double)m_oSize.height*m_oSize.width*m_nTotFrameCount*(int(!m_bForcingGrayscale)+1);
    }
    else if(m_eDatasetID==eDataset_LITIV2012) {
        PlatformUtils::GetFilesFromDir(m_sPath+"/input/",m_vsInputFramePaths);
        if(m_vsInputFramePaths.empty())
            throw std::runtime_error(std::string("Sequence at ") + m_sPath + " did not possess any parsable input images");
        cv::Mat oTempImg = cv::imread(m_vsInputFramePaths[0]);
        if(oTempImg.empty())
            throw std::runtime_error(std::string("Bad image file ('")+m_vsInputFramePaths[0]+"'), could not be read");
        /*m_voVideoReader.open(m_sPath+"/input/in%06d.jpg");
        if(!m_voVideoReader.isOpened())
            m_voVideoReader.open(m_sPath+"/"+m_sName+".avi");
        if(!m_voVideoReader.isOpened())
            throw std::runtime_error(std::string("Bad video file ('")+m_sPath+std::string("'), could not be opened"));
        m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,0);
        cv::Mat oTempImg;
        m_voVideoReader >> oTempImg;
        m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,0);
        if(oTempImg.empty())
            throw std::runtime_error(std::string("Bad video file ('")+m_sPath+std::string("'), could not be read"));*/
        m_oROI = cv::Mat(oTempImg.size(),CV_8UC1,cv::Scalar_<uchar>(255));
        m_oSize = oTempImg.size();
        m_nNextExpectedVideoReaderFrameIdx = (size_t)-1;
        //m_nTotFrameCount = (size_t)m_voVideoReader.get(cv::CAP_PROP_FRAME_COUNT);
        m_nTotFrameCount = m_vsInputFramePaths.size();
        CV_Assert(m_nTotFrameCount>0);
        m_dExpectedLoad = (double)m_oSize.height*m_oSize.width*m_nTotFrameCount*(int(!m_bForcingGrayscale)+1);
    }
    else if(m_eDatasetID==eDataset_GenericTest) {
        m_voVideoReader.open(m_sPath);
        if(!m_voVideoReader.isOpened())
            throw std::runtime_error(std::string("Bad video file ('")+m_sPath+std::string("'), could not be opened"));
        m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,0);
        cv::Mat oTempImg;
        m_voVideoReader >> oTempImg;
        m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,0);
        if(oTempImg.empty())
            throw std::runtime_error(std::string("Bad video file ('")+m_sPath+std::string("'), could not be read"));
        m_oROI = cv::Mat(oTempImg.size(),CV_8UC1,cv::Scalar_<uchar>(255));
        m_oSize = oTempImg.size();
        m_nNextExpectedVideoReaderFrameIdx = 0;
        m_nTotFrameCount = (size_t)m_voVideoReader.get(cv::CAP_PROP_FRAME_COUNT);
        CV_Assert(m_nTotFrameCount>0);
        m_dExpectedLoad = (double)m_oSize.height*m_oSize.width*m_nTotFrameCount*(int(!m_bForcingGrayscale)+1);
    }
    else
        throw std::runtime_error(std::string("Unknown dataset type, cannot use any known parsing strategy"));
}

cv::Mat DatasetUtils::Segm::Video::SequenceInfo::GetInputFromIndex_external(size_t nFrameIdx) {
    cv::Mat oFrame;
    if( m_eDatasetID==eDataset_CDnet2012 ||
        m_eDatasetID==eDataset_CDnet2014 ||
        m_eDatasetID==eDataset_Wallflower ||
        m_eDatasetID==eDataset_LITIV2012)
        oFrame = cv::imread(m_vsInputFramePaths[nFrameIdx],m_nIMReadInputFlags);
    else if( m_eDatasetID==eDataset_PETS2001_D3TC1 ||
            /*m_eDatasetID==eDataset_LITIV2012 || */
            m_eDatasetID==eDataset_GenericTest) {
        if(m_nNextExpectedVideoReaderFrameIdx!=nFrameIdx) {
            m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,(double)nFrameIdx);
            m_nNextExpectedVideoReaderFrameIdx = nFrameIdx+1;
        }
        else
            ++m_nNextExpectedVideoReaderFrameIdx;
        m_voVideoReader >> oFrame;
        if(m_bForcingGrayscale && oFrame.channels()>1)
            cv::cvtColor(oFrame,oFrame,cv::COLOR_BGR2GRAY);
    }
    if(m_bUsing4chAlignment && oFrame.channels()==3)
        cv::cvtColor(oFrame,oFrame,cv::COLOR_BGR2BGRA);
    CV_Assert(oFrame.size()==m_oSize);
#if DATASETUTILS_HARDCODE_FRAME_INDEX
    std::stringstream sstr;
    sstr << "Frame #" << nFrameIdx;
    WriteOnImage(oFrame,sstr.str(),cv::Scalar_<uchar>::all(255);
#endif //DATASETUTILS_HARDCODE_FRAME_INDEX
    return oFrame;
}

cv::Mat DatasetUtils::Segm::Video::SequenceInfo::GetGTFromIndex_external(size_t nFrameIdx) {
    cv::Mat oFrame;
    if(m_eDatasetID == eDataset_CDnet2012 ||
       m_eDatasetID == eDataset_CDnet2014)
        oFrame = cv::imread(m_vsGTFramePaths[nFrameIdx],cv::IMREAD_GRAYSCALE);
    else if(m_eDatasetID == eDataset_Wallflower ||
            m_eDatasetID == eDataset_PETS2001_D3TC1) {
        auto res = m_mTestGTIndexes.find(nFrameIdx);
        if(res != m_mTestGTIndexes.end())
            oFrame = cv::imread(m_vsGTFramePaths[res->second],cv::IMREAD_GRAYSCALE);
    }
    if(oFrame.empty())
        oFrame = cv::Mat(m_oSize,CV_8UC1,cv::Scalar_<uchar>(DATASETUTILS_OUT_OF_SCOPE_DEFAULT_VAL));
    CV_Assert(oFrame.size()==m_oSize);
#if DATASETUTILS_HARDCODE_FRAME_INDEX
    std::stringstream sstr;
    sstr << "Frame #" << nFrameIdx;
    WriteOnImage(oFrame,sstr.str(),cv::Scalar_<uchar>::all(255);
#endif //DATASETUTILS_HARDCODE_FRAME_INDEX
    return oFrame;
}
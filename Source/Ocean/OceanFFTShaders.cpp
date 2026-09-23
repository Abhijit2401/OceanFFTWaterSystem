#include "OceanFFTShaders.h"

IMPLEMENT_GLOBAL_SHADER(FOceanInitSpectrumCS, "/Project/Private/Ocean/OceanFFT.usf", "InitSpectrumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FOceanTimeSpectrumCS, "/Project/Private/Ocean/OceanFFT.usf", "TimeSpectrumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FOceanFFTButterflyCS, "/Project/Private/Ocean/OceanFFT.usf", "FFTButterflyCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FOceanInversionCS, "/Project/Private/Ocean/OceanFFT.usf", "InversionCS", SF_Compute);

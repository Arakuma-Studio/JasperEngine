/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.

--

Based on "Using the Biquad Resonant Filter", 
Phil Burk, Game Programming Gems 3, p. 606
*/

#include <math.h>
#include <string.h>
#include "soloud.h"
#include "soloud_biquadresonantfilter.h"

namespace SoLoud
{
	void BiquadResonantFilterInstance::calcBQRParams()
	{
		mDirty = 0;

		float omega = (float)((2.0f * M_PI * mParam[FREQUENCY]) / mSamplerate);
		float sin_omega = (float)sin(omega);
		float cos_omega = (float)cos(omega);
		float alpha = sin_omega / (2.0f * mParam[RESONANCE]);
		float scalar = 1.0f / (1.0f + alpha);

		switch ((int)(mParam[TYPE]))
		{
		default:
		case BiquadResonantFilter::LOWPASS:
			mA0 = 0.5f * (1.0f - cos_omega) * scalar;
			mA1 = (1.0f - cos_omega) * scalar;
			mA2 = mA0;
			mB1 = -2.0f * cos_omega * scalar;
			mB2 = (1.0f - alpha) * scalar;
			break;
		case BiquadResonantFilter::HIGHPASS:
			mA0 = 0.5f * (1.0f + cos_omega) * scalar;
			mA1 = -(1.0f + cos_omega) * scalar;
			mA2 = mA0;
			mB1 = -2.0f * cos_omega * scalar;
			mB2 = (1.0f - alpha) * scalar;
			break;
		case BiquadResonantFilter::BANDPASS:
			mA0 = alpha * scalar;
			mA1 = 0;
			mA2 = -mA0;
			mB1 = -2.0f * cos_omega * scalar;
			mB2 = (1.0f - alpha) * scalar;
			break;
		}
	}


	BiquadResonantFilterInstance::BiquadResonantFilterInstance(BiquadResonantFilter *aParent)
	{
		int i;
		for (i = 0; i < 8; i++)
		{
			mState[i].mX1 = 0;
			mState[i].mY1 = 0;
			mState[i].mX2 = 0;
			mState[i].mY2 = 0;
		}

		mParent = aParent;

		initParams(4);
		
		mParam[RESONANCE] = aParent->mResonance;
		mParam[FREQUENCY] = aParent->mFrequency;
		mParam[TYPE] = (float)aParent->mFilterType;
		
		mSamplerate = 44100;

		calcBQRParams();
	}

	void BiquadResonantFilterInstance::filterChannel(float *aBuffer, unsigned int aSamples, float aSamplerate, double aTime, unsigned int aChannel, unsigned int /*aChannels*/)
	{
		unsigned int osamples = aSamples;
		if (aChannel == 0)
		{
			updateParams(aTime);

			if (mParamChanged & ((1 << FREQUENCY) | (1 << RESONANCE) | (1 << TYPE)) || aSamplerate != mSamplerate)
			{
				mSamplerate = aSamplerate;
				calcBQRParams();
			}
			mParamChanged = 0;			
		}		
		float x;
		unsigned int i;
		int c = 0;

		BQRStateData &s = mState[aChannel];

		// make sure we access pairs of samples (one sample may be skipped)
		aSamples = aSamples & ~1; 

		// JasperEngine: the coefficients and the state in locals (the same arithmetic in the same
		// order): aBuffer may alias the members, so the loop reloaded and stored them every sample
		const float a0 = mA0, a1 = mA1, a2 = mA2, b1 = mB1, b2 = mB2, wet = mParam[WET];
		float x1 = s.mX1, x2 = s.mX2, y1 = s.mY1, y2 = s.mY2;
		for (i = 0; i < aSamples; i +=2, c++)
		{
			// Generate outputs by filtering inputs.
			x = aBuffer[c];
			y2 = (a0 * x) + (a1 * x1) + (a2 * x2) - (b1 * y1) - (b2 * y2);
			aBuffer[c] += (y2 - aBuffer[c]) * wet;

			c++;

			// Permute filter operations to reduce data movement.
			// Just substitute variables instead of doing mX1=x, etc.
			x2 = aBuffer[c];
			y1 = (a0 * x2) + (a1 * x) + (a2 * x1) - (b1 * y2) - (b2 * y1);
			aBuffer[c] += (y1 - aBuffer[c]) * wet;

			// Only move a little data.
			x1 = x2;
			x2 = x;
		}
		s.mX1 = x1;
		s.mX2 = x2;
		s.mY1 = y1;
		s.mY2 = y2;
		// If we skipped a sample earlier, patch it by just copying the previous.
		if (osamples != aSamples)
			aBuffer[c] = aBuffer[c - 1];
	}


	BiquadResonantFilterInstance::~BiquadResonantFilterInstance()
	{
	}

	BiquadResonantFilter::BiquadResonantFilter()
	{
		setParams(LOWPASS, 1000, 2);
	}

	result BiquadResonantFilter::setParams(int aType, float aFrequency, float aResonance)
	{
		if (aType < 0 || aType > 3 || aFrequency <= 0 || aResonance <= 0)
			return INVALID_PARAMETER;

		mFilterType = aType;
		mFrequency = aFrequency;
		mResonance = aResonance;

		return 0;
	}

	int BiquadResonantFilter::getParamCount()
	{
		return 4;
	}

	const char* BiquadResonantFilter::getParamName(unsigned int aParamIndex)
	{
		if (aParamIndex > 3)
			return 0;

		const char* name[4] = {
			"Wet",
			"Type",
			"Frequency",
			"Resonance"
		};
		return name[aParamIndex];
	}
	
	unsigned int BiquadResonantFilter::getParamType(unsigned int aParamIndex)
	{
		if (aParamIndex == TYPE)
			return INT_PARAM;
		return FLOAT_PARAM;
	}

	float BiquadResonantFilter::getParamMax(unsigned int aParamIndex)
	{
		switch (aParamIndex)
		{
		case WET: return 1;
		case TYPE: return 2;
		case FREQUENCY: return 8000;
		case RESONANCE: return 20;
		}
		return 1;
	}
	
	float BiquadResonantFilter::getParamMin(unsigned int aParamIndex)
	{
		switch (aParamIndex)
		{
		case FREQUENCY: return 10;
		case RESONANCE: return 0.1f;
		}
		return 0;
	}


	BiquadResonantFilter::~BiquadResonantFilter()
	{
	}


	BiquadResonantFilterInstance *BiquadResonantFilter::createInstance()
	{
		return new BiquadResonantFilterInstance(this);
	}
}

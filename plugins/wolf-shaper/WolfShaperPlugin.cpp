/*
 * Wolf Shaper audio effect based on DISTRHO Plugin Framework (DPF)
 *
 * SPDX-License-Identifier: GPL v3+
 *
 * Copyright (C) 2021 Patrick Desaulniers
 */

#include "DistrhoPlugin.hpp"
#include "extra/Mutex.hpp"
#include "extra/ScopedPointer.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cmath>

#include "WolfShaperParameters.hpp"
#include "Graph.hpp"
#include "Oversampler.hpp"
#include "Mathf.hpp"
#include "ParamSmooth.hpp"

#include "DspFilters/Dsp.h"

START_NAMESPACE_DISTRHO


class WolfShaper : public Plugin
{
  public:
	WolfShaper() : Plugin(paramCount, 0, 1),
				   oversampler(),
				   mustCopyLineEditor(false),
				   inputIndicatorPos(0.0f),
				   inputIndicatorAcceleration(0.0f),
				   removeDCPrev{0.f, 0.f}
	{
	}

  protected:
	const char *getLabel() const noexcept override
	{
		return "Wolf Shaper";
	}

	const char *getDescription() const noexcept override
	{
		return "Waveshaping distortion plugin with spline-based graph.";
	}

	const char *getMaker() const noexcept override
	{
		return "Patrick Desaulniers";
	}

	const char *getHomePage() const noexcept override
	{
		return "https://github.com/pdesaulniers/wolf-shaper";
	}

	const char *getLicense() const noexcept override
	{
		return "GPL v3+";
	}

	uint32_t getVersion() const noexcept override
	{
		return d_version(0, 1, 8);
	}

	int64_t getUniqueId() const noexcept override
	{
		return d_cconst('s', 'W', 's', 'p');
	}

	void initParameter(uint32_t index, Parameter &parameter) override
	{
		if (index >= paramCount)
			return;

		switch (index)
		{
		case paramPreGain:
			parameter.name = "Pre Gain";
			parameter.symbol = "pregain";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 2.0f;
			parameter.ranges.def = 1.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsLogarithmic;
			break;
		case paramWet:
			parameter.name = "Wet";
			parameter.symbol = "wet";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 1.0f;
			parameter.ranges.def = 1.0f;
			parameter.hints = kParameterIsAutomable;
			break;
		case paramPostGain:
			parameter.name = "Post Gain";
			parameter.symbol = "postgain";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 1.0f;
			parameter.ranges.def = 1.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsLogarithmic;
			break;
		case paramRemoveDC:
			parameter.name = "Remove DC Offset";
			parameter.symbol = "removedc";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 1.0f;
			parameter.ranges.def = 1.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsBoolean | kParameterIsInteger;
			break;
		case paramOversample:
			//None, 2x, 4x, 8x, 16x
			parameter.name = "Oversample";
			parameter.symbol = "oversample";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 4.0f;
			parameter.ranges.def = 0.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsInteger;
			break;
		case paramBipolarMode:
			parameter.name = "Bipolar Mode";
			parameter.symbol = "bipolarmode";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 1.0f;
			parameter.ranges.def = 0.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsBoolean | kParameterIsInteger;
			break;
		case paramHorizontalWarpType:
			//None, Bend +, Bend -, Bend +/-, Skew +, Skew -, Skew +/-
			parameter.name = "H Warp Type";
			parameter.symbol = "warptype";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 6.0f;
			parameter.ranges.def = 0.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsInteger;
			break;
		case paramHorizontalWarpAmount:
			parameter.name = "H Warp Amount";
			parameter.symbol = "warpamount";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 1.0f;
			parameter.ranges.def = 0.0f;
			parameter.hints = kParameterIsAutomable;
			break;
		case paramVerticalWarpType:
			//None, Bend +, Bend -, Bend +/-, Skew +, Skew -, Skew +/-
			parameter.name = "V Warp Type";
			parameter.symbol = "vwarptype";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 6.0f;
			parameter.ranges.def = 0.0f;
			parameter.hints = kParameterIsAutomable | kParameterIsInteger;
			break;
		case paramVerticalWarpAmount:
			parameter.name = "V Warp Amount";
			parameter.symbol = "vwarpamount";
			parameter.ranges.min = 0.0f;
			parameter.ranges.max = 1.0f;
			parameter.ranges.def = 0.0f;
			parameter.hints = kParameterIsAutomable;
			break;
		case paramOut:
			parameter.name = "Out";
			parameter.symbol = "out";
			parameter.hints = kParameterIsOutput;
			parameter.ranges.def = 0.0f;
			break;
		}

		parameters[index] = ParamSmooth(parameter.ranges.def);
		parameters[index].calculateCoeff(20.f, getSampleRate());
	}

	float getParameterValue(uint32_t index) const override
	{
		return parameters[index].getRawValue();
	}

	int getOversamplingRatio()
	{
		return std::pow(2, std::round(parameters[paramOversample].getRawValue()));
	}

	void setParameterValue(uint32_t index, float value) override
	{
		parameters[index].setValue(value);

		if (index == paramOversample)
		{
			for (int i = 0; i < paramCount; ++i)
			{
				parameters[i].calculateCoeff(20.f, getSampleRate() * getOversamplingRatio());
			}
		}
	}

	void initState(uint32_t index, String &stateKey, String &defaultStateValue) override
	{
		switch (index)
		{
		case 0:
			stateKey = "graph";
			break;
		}

		//generated with fprintf(stderr, "%A,%A,%A,%d;%A,%A,%A,%d;\n", 0.0f, 0.0f, 0.0f, wolf::CurveType::Exponential, 1.0f, 1.0f, 0.0f, wolf::CurveType::Exponential);
		defaultStateValue = String("0x0p+0,0x0p+0,0x0p+0,0;0x1p+0,0x1p+0,0x0p+0,0;");
	}

	void setState(const char *key, const char *value) override
	{
		const MutexLocker cml(mutex);

		if (std::strcmp(key, "graph") == 0)
		{
			tempLineEditor.rebuildFromString(value);
			mustCopyLineEditor = true;
		}
	}

	float removeDCOffset(float input, int channel)
	{
		//Steep IIR filter at the DC frequency

		const float scaleFactor = 0.9999f; //Closer to 1 means steeper stop band

		const float value = input + scaleFactor * removeDCPrev[channel];
		const float result = value - removeDCPrev[channel];

		removeDCPrev[channel] = value;

		return result;
	}

	float calculateValueOutsideGraph(float value)
	{
		const bool bipolarMode = lineEditor.getBipolarMode();

		if (bipolarMode)
		{
			const bool positiveInput = value >= 0.0f;
			const float vertexY = lineEditor.getVertexAtIndex(positiveInput ? lineEditor.getVertexCount() - 1 : 0)->getY();
			const float absValue = std::abs(value);

			return absValue * (-1.0f + vertexY * 2.0f);
		}
		else
		{
			return value * lineEditor.getVertexAtIndex(lineEditor.getVertexCount() - 1)->getY();
		}
	}

	float getGraphValue(float input)
	{
		const float absInput = std::abs(input);

		if (absInput > 1.0f)
		{
			return calculateValueOutsideGraph(input);
		}

		const bool bipolarMode = lineEditor.getBipolarMode();

		if (bipolarMode)
		{
			const float x = (1.0f + input) * 0.5f;
			return -1.0f + lineEditor.getValueAt(x) * 2.0f;
		}
		else
		{
			return lineEditor.getValueAt(input);
		}
	}

	void updateInputIndicatorPos(const float peak, const uint32_t frames)
	{
		const float deadZone = 0.001f;
		const float speed = 0.35f;

		if (peak > deadZone && peak > inputIndicatorPos)
		{
			inputIndicatorPos = peak;
			inputIndicatorAcceleration = 0.0f;
		}
		else if (inputIndicatorPos > -deadZone)
		{
			inputIndicatorPos -= inputIndicatorAcceleration * frames;
			inputIndicatorAcceleration += std::pow(inputIndicatorAcceleration + speed / getSampleRate(), 2) * frames;
		}

		inputIndicatorPos = wolf::clamp(inputIndicatorPos, -deadZone, 1.0f);
	}

	void run(const float **inputs, float **outputs, uint32_t frames) override
	{
		const auto lockSucceeded = mutex.tryLock();

		if (lockSucceeded)
		{
			if (mustCopyLineEditor)
			{
				lineEditor = tempLineEditor;

				for (int i = 0; i < lineEditor.getVertexCount(); ++i)
				{
					lineEditor.getVertexAtIndex(i)->setGraphPtr(&lineEditor);
				}

				mustCopyLineEditor = false;
			}
		}

		float peak = 0.0f;

		const int oversamplingRatio = getOversamplingRatio();
		uint32_t numSamples = frames * oversamplingRatio;

		const double sampleRate = getSampleRate();

		float **buffer = oversampler.upsample(oversamplingRatio, frames, sampleRate, inputs);

		wolf::WarpType horizontalWarpType = (wolf::WarpType)std::round(parameters[paramHorizontalWarpType].getRawValue());
		lineEditor.setHorizontalWarpType(horizontalWarpType);

		wolf::WarpType verticalWarpType = (wolf::WarpType)std::round(parameters[paramVerticalWarpType].getRawValue());
		lineEditor.setVerticalWarpType(verticalWarpType);

		const bool mustRemoveDC = parameters[paramRemoveDC].getRawValue() > 0.50f;

		if (!mustRemoveDC)
		{
			removeDCPrev[0] = 0.f;
			removeDCPrev[1] = 0.f;
		}

		for (uint32_t i = 0; i < numSamples; ++i)
		{
			lineEditor.setHorizontalWarpAmount(parameters[paramHorizontalWarpAmount].getSmoothedValue());
			lineEditor.setVerticalWarpAmount(parameters[paramVerticalWarpAmount].getSmoothedValue());

			const float preGain = parameters[paramPreGain].getSmoothedValue();

			float inputL = preGain * buffer[0][i];
			float inputR = preGain * buffer[1][i];

			//this hack makes sure that dc offset in the graph doesn't oscillate... hopefully
			if (inputL < 0.0f && inputL > -0.00001f)
			{
				inputL = 0.0f;
			}
			if (inputR < 0.0f && inputR > -0.00001f)
			{
				inputR = 0.0f;
			}

			const float absL = std::abs(inputL);
			const float absR = std::abs(inputR);

			peak = std::max(peak, absL);
			peak = std::max(peak, absR);

			const bool bipolarMode = parameters[paramBipolarMode].getRawValue() > 0.50f;
			lineEditor.setBipolarMode(bipolarMode);

			float graphL, graphR;

			graphL = getGraphValue(inputL);
			graphR = getGraphValue(inputR);

			const float wet = parameters[paramWet].getSmoothedValue();
			const float dry = 1.0f - wet;
			const float postGain = parameters[paramPostGain].getSmoothedValue();

			buffer[0][i] = (dry * inputL + wet * graphL) * postGain;
			buffer[1][i] = (dry * inputR + wet * graphR) * postGain;

			if (mustRemoveDC)
			{
				buffer[0][i] = removeDCOffset(buffer[0][i], 0);
				buffer[1][i] = removeDCOffset(buffer[1][i], 1);
			}
		}

		oversampler.downsample(outputs);

		updateInputIndicatorPos(peak, frames);
		setParameterValue(paramOut, inputIndicatorPos);

		if (lockSucceeded)
		{
			mutex.unlock();
		}
	}

  private:
	ParamSmooth parameters[paramCount];
	Oversampler oversampler;

	wolf::Graph lineEditor;
	wolf::Graph tempLineEditor;
	bool mustCopyLineEditor;

	float inputIndicatorPos;
	float inputIndicatorAcceleration;

	Mutex mutex;

	float removeDCPrev[2];

	DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WolfShaper)
};

Plugin *createPlugin()
{
	return new WolfShaper();
}

END_NAMESPACE_DISTRHO
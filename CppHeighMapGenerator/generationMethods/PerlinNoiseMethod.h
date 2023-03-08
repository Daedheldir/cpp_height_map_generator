#pragma once
#include "GenerationMethodBase.h"

class PerlinNoiseMethod : public GenerationMethodBase
{
public:
	PerlinNoiseMethod(GenerationSettings settings, int seed, float scaleOverride);
	virtual ~PerlinNoiseMethod();

private:
	// Inherited via GenerationMethodBase
	virtual float EvaluateHeight(const std::pair<float, float>& point) override;

private:
};

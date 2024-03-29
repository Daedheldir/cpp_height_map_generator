﻿// CppHeighMapGenerator.cpp : Defines the entry point for the application.
//

#include "CppHeightMapGenerator.h"

#include "generationMethods/PerlinNoiseMethod.hpp"
#include "generationMethods/SineNoiseMethod.hpp"
#include "generationMethods/CosineNoiseMethod.hpp"
#include "generationMethods/RidgedNoiseMethod.hpp"
#include "generationMethods/BillowNoiseMethod.hpp"
#include "utilities/BMPHandler.h"
#include "utilities/pnm.hpp"
#include "utilities/IniHandler.hpp"
#include "utilities/Configuration.hpp"
#include "utilities/MathOperations.hpp"
#include "utilities/CsvHandler.hpp"
#include "utilities/MemoryProfiling.hpp"

#include <limits>
#include <cmath>
#include <chrono>
#include <thread>



using HeightMap = std::vector<std::vector<float>>;

void PopulateGenerationSettingsForConfig(IniHandler& confHandler, dh::Config::Configuration& config) {
	const size_t mapSize = config.mapSize;

	using namespace dh::Config;

	IniHandler::Property* methodProperty = confHandler.GetProperty(std::string(Categories::generationMethods));
	if (methodProperty == nullptr)
		return;

	config.generationSettingsArr.clear();

	for (auto& prop : methodProperty->subproperties) {
		GenerationSettings::GenerationMethodType methodType;

		auto GetSubpropProperty = [&prop, &confHandler](const std::string_view& key) {
			return confHandler.GetPropertyValue(prop.second.subproperties, std::string(key));
		};

		if (prop.first == Categories::methodType_sine)
			methodType = GenerationSettings::GenerationMethodType::Sine;
		else if (prop.first == Categories::methodType_cosine)
			methodType = GenerationSettings::GenerationMethodType::Cosine;
		else if (prop.first == Categories::methodType_ridged)
			methodType = GenerationSettings::GenerationMethodType::Ridged;
		else if (prop.first == Categories::methodType_billow)
			methodType = GenerationSettings::GenerationMethodType::Billow;
		else
			methodType = GenerationSettings::GenerationMethodType::PerlinNoise;

		bool useFirstOctaveAsMask = false;
		std::istringstream(GetSubpropProperty(Method::useFirstOctaveAsMask)) >> std::boolalpha >> useFirstOctaveAsMask;

		bool useFirstHeightMapsAsMask = false;
		std::istringstream(GetSubpropProperty(Method::useFirstHeightMapsAsMask)) >> std::boolalpha >> useFirstHeightMapsAsMask;

		bool invertFirstHeightMapMask = false;
		std::istringstream(GetSubpropProperty(Method::invertFirstHeightMapMask)) >> std::boolalpha >> invertFirstHeightMapMask;

		bool subtractFromMap = false;
		std::istringstream(GetSubpropProperty(Method::subtractFromMap)) >> std::boolalpha >> subtractFromMap;

		const int octaves = std::stoi(GetSubpropProperty(Method::octaves));
		const float scale = std::stof(GetSubpropProperty(Method::scale));
		const float weight = std::stof(GetSubpropProperty(Method::weight));
		const float persistance = std::stof(GetSubpropProperty(Method::persistance));
		const float smoothing = std::stof(GetSubpropProperty(Method::smoothing));

		auto GetRandInRange = [](float min, float max, float precision) {
			int iMin = static_cast<int>(min * (1.0f / precision));
			int iMax = static_cast<int>(max * (1.0f / precision));
			if (iMin - iMax == 0) return 0.0f;

			return (iMin + rand() % (iMax - iMin)) * precision;
		};
		int octavesRandomization = GetRandInRange(-config.octavesRandomizationFactor, config.octavesRandomizationFactor, 1.0f);
		float scaleRandomization = GetRandInRange(-config.scaleRandomizationFactor, config.scaleRandomizationFactor, 0.01f);
		float weightRandomization = GetRandInRange(-config.weightRandomizationFactor, config.weightRandomizationFactor, 0.01f);
		float persistanceRandomization = GetRandInRange(-config.persistanceRandomizationFactor, config.persistanceRandomizationFactor, 0.01f);
		float smoothingRandomization = GetRandInRange(-config.smoothingRandomizationFactor, config.smoothingRandomizationFactor, 0.01f);

		GenerationSettings settings(
			methodType,
			std::clamp(octaves + octavesRandomization, 0, 12),
			std::clamp(scale + scaleRandomization, 1.0f, 1000.0f),
			std::clamp(weight + weightRandomization, 0.1f, 10.0f),
			std::clamp(persistance + persistanceRandomization, 0.1f, 1.0f),
			std::clamp(smoothing + smoothingRandomization, 0.1f, 1.0f),
			mapSize
		);

		if (config.randomizeActiveLayers && rand() % 10 > 6) {
			settings.isActive(false);
			continue;
		}

		if (config.randomizeMasks) {
			useFirstOctaveAsMask = (rand() % 2) != 0;
			useFirstHeightMapsAsMask = (rand() % 2) != 0;
			invertFirstHeightMapMask = (rand() % 2) != 0;
			subtractFromMap = (rand() % 2) != 0;
		}
		settings.isFirstOctaveAsMask(useFirstOctaveAsMask);
		settings.isFirstHeightMapAsMask(useFirstHeightMapsAsMask);
		settings.isInvertFirstHeightMapMask(invertFirstHeightMapMask);
		settings.isSubtractFromMap(subtractFromMap);

		config.generationSettingsArr.emplace_back(settings);
	}
}
HeightMap NormalizeMap(const HeightMap& inputMap, const float minValue, const float maxValue) {
	std::cout << "Normalizing map..." << std::endl;

	HeightMap map{ inputMap.size(), std::vector<float>(inputMap.size(), 0.0f) };

	for (int y = 0; y < inputMap.size(); ++y)
	{
		for (int x = 0; x < inputMap.size(); ++x)
		{
			auto InverseLerp = [](float xx, float yy, float value)
			{
				return (value - xx) / (yy - xx);
			};

			float value = InverseLerp(minValue, maxValue, inputMap[x][y]);
			map[x][y] = value;
		}
	}
	return map;
}

HeightMap SumGeneratedHeightMaps(const std::vector< HeightMap>& heightMaps, const std::vector<GenerationSettings>& generationSettings) {
	std::cout << "Merging generated maps..." << std::endl;
	HeightMap map;
	map.resize(generationSettings[0].getChunkSize(),
		std::vector<float>(generationSettings[0].getChunkSize(), 0.0f));

	const HeightMap& globalFirstOctaveMask = heightMaps[0];

	float minValue = 10000.0f;
	float maxValue = -10000.0f;

	for (int i = 0; i < heightMaps.size(); ++i) {
		std::cout << "\tMerging map " << (i + 1) << "..." << std::endl;

		for (int y = 0; y < map.size(); ++y) {
			for (int x = 0; x < map.size(); ++x) {
				const HeightMap& tempMap = heightMaps[i];

				float addedVal;
				if (generationSettings[i].isFirstHeightMapAsMask())
				{
					addedVal = tempMap[x][y]
						* generationSettings[i].getWeight()
						* (generationSettings[i].isInvertFirstHeightMapMask() ? 1 - globalFirstOctaveMask[x][y] : globalFirstOctaveMask[x][y]);
				}
				else
					addedVal = tempMap[x][y] * generationSettings[i].getWeight();

				map[x][y] += generationSettings[i].isSubtractFromMap() ? -addedVal : addedVal;

				if (map[x][y] > maxValue)
					maxValue = map[x][y];
				else if (map[x][y] < minValue)
					minValue = map[x][y];
			}
		}
	}

	return NormalizeMap(map, minValue, maxValue);
}

HeightMap GenerateMap(const dh::Config::Configuration& config) {
	std::vector<HeightMap> maps;
	int counter = 1;
	for (const auto& genSettings : config.generationSettingsArr) {
		//std::cout << "\tGenerating map " << counter++ << "..." << std::endl;
		GenerationMethodBase* generationBasePointer;
		switch (genSettings.getMethodType())
		{
		case GenerationSettings::GenerationMethodType::Sine:
			generationBasePointer = new SineNoiseMethod(genSettings, 0, 1.0f);
			break;
		case GenerationSettings::GenerationMethodType::Cosine:
			generationBasePointer = new CosineNoiseMethod(genSettings, 0, 1.0f);
			break;
		case GenerationSettings::GenerationMethodType::Ridged:
			generationBasePointer = new RidgedNoiseMethod(genSettings, 0, 1.0f);
			break;
		case GenerationSettings::GenerationMethodType::Billow:
			generationBasePointer = new BillowNoiseMethod(genSettings, 0, 1.0f);
			break;
		default:
			generationBasePointer = new PerlinNoiseMethod(genSettings, 0, 1.0f);
			break;
		}

		maps.emplace_back(generationBasePointer->CreateHeightMap(std::pair<float, float>(0.0f, 0.0f)));
	}

	return SumGeneratedHeightMaps(maps, config.generationSettingsArr);
}
std::vector<std::string> CalculateExecutionTimesStatistics(const int iter, const std::vector<double>& executionTimes) {
	double max = *std::max_element(std::begin(executionTimes), std::end(executionTimes));
	double min = *std::min_element(std::begin(executionTimes), std::end(executionTimes));

	double mean = dh::Math::CalculateMean(executionTimes);
	double variance = dh::Math::CalculateVariance(executionTimes);
	double stdDev = dh::Math::CalculateStandardDeviation(executionTimes);

	std::cout << "Max: " << max / 1000000 << " ms, Min: " << min / 1000000 << " ms, Mean: " << mean / 1000000 << " ms, variance: " << variance / std::pow(1000000, 2) << " ms^2, stddev: " << stdDev / 1000000 << "ms" << std::endl;

	auto roundPlaces = [](double number, int places) {
		const double multiplier = std::pow(10.0, places);
		return std::round(number * multiplier) / multiplier;
	};
	constexpr int decimalPlaces = 5;
	std::vector<std::string> row = { std::to_string(iter),
		std::to_string(roundPlaces(max / 1000000, decimalPlaces)),
		std::to_string(roundPlaces(mean / 1000000, decimalPlaces)),
		std::to_string(roundPlaces(min / 1000000, decimalPlaces)) ,
		std::to_string(roundPlaces(variance / std::pow(1000000, 2), decimalPlaces)),
		std::to_string(roundPlaces(stdDev / 1000000, decimalPlaces)) };

	return row;
}
//the magic number 7+-2 - millers effect
int main()
{
	std::cout << "Opening configuration file." << std::endl;
	IniHandler confHandler("config.ini");

	dh::Config::Configuration configuration;

	using namespace dh::Config;
	configuration.mapSize = std::stol(confHandler.GetPropertyValue(std::string(Map::mapSize)));
	configuration.iterations = std::stol(confHandler.GetPropertyValue(std::string(Map::iterations)));

	std::istringstream(confHandler.GetPropertyValue(std::string(Map::randomizeMasks)))
		>> std::boolalpha >> configuration.randomizeMasks;
	std::istringstream(confHandler.GetPropertyValue(std::string(Map::randomizeActiveLayers)))
		>> std::boolalpha >> configuration.randomizeActiveLayers;

	configuration.octavesRandomizationFactor = std::stol(confHandler.GetPropertyValue(std::string(Map::octavesRandomizationFactor)));
	configuration.scaleRandomizationFactor = std::stof(confHandler.GetPropertyValue(std::string(Map::scaleRandomizationFactor)));
	configuration.weightRandomizationFactor = std::stof(confHandler.GetPropertyValue(std::string(Map::weightRandomizationFactor)));
	configuration.persistanceRandomizationFactor = std::stof(confHandler.GetPropertyValue(std::string(Map::persistanceRandomizationFactor)));
	configuration.smoothingRandomizationFactor = std::stof(confHandler.GetPropertyValue(std::string(Map::smoothingRandomizationFactor)));

	// TODO: Disable automatic core optimizations in BIOS
	// TODO: Lock program to a single core
	// TODO: Add a .csv file export with all parameters and test results

	CsvHandler csvHandler;
	std::vector<std::string> columnNames = { "Iteration", "Max", "Mean", "Min", "Variance", "Std Deviation" };
	csvHandler.AddRowToCsv(columnNames);

	std::vector<double> executionTimes;


	std::cout << "Starting maps generation..." << std::endl;

	for (int iter = 0; iter < configuration.iterations; ++iter) {
		PopulateGenerationSettingsForConfig(confHandler, configuration);

		std::cout << "Iteration " << iter + 1 << std::endl;
		std::cout << "Generating maps..." << std::endl;

		auto pmc = MemoryProfiler::GetMemoryUsage();
		long privUsage = pmc.PagefileUsage / 1024;
		long workingSet = pmc.WorkingSetSize / 1024;
		std::cout << "Pagefile:" << privUsage << "kB" << std::endl;
		std::cout << "Working set: " << workingSet << "kB" << std::endl;

		auto start = std::chrono::high_resolution_clock::now();
		HeightMap map = GenerateMap(configuration);
		auto end = std::chrono::high_resolution_clock::now();

		pmc = MemoryProfiler::GetMemoryUsage();
	
		std::cout << "Pagefile:" << pmc.PagefileUsage / 1024 << "kB" << "(+" << (pmc.PagefileUsage / 1024) - privUsage << "kB)" << std::endl;
		std::cout << "Working set: " << pmc.WorkingSetSize / 1024 << "kB" << "(+" << (pmc.WorkingSetSize / 1024) - workingSet << "kB)" << std::endl;

		executionTimes.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

		std::cout << "Done!" << std::endl;

		const size_t height = configuration.mapSize;
		const size_t width = configuration.mapSize;

		pnm::image<pnm::gray_pixel> img(width, height);
		std::ostringstream ostr;
		ostr << "img/heightMap_" << iter << ".pgm";
		std::string imageFileName = ostr.str();

		std::filesystem::path filedir(imageFileName);
		if (filedir.has_parent_path()) {
			if (!std::filesystem::exists(filedir.parent_path())) {
				std::filesystem::create_directory(filedir.parent_path());
			}
		}

		for (int i = 0; i < height; i++) {
			for (int j = 0; j < width; j++) {
				if (map[i][j] > 1.0f || map[i][j] < 0.0f)
					std::cerr << "Warning! Map value ouside [0,1]" << std::endl;
				img[i][j] = static_cast<unsigned char>(std::clamp(map[i][j] * 255, 0.f, 255.f));
			}
		}

		pnm::write_pgm_binary(imageFileName, img);
	}
	std::cout << "\tCalculating statistics..." << std::endl;
	auto row = CalculateExecutionTimesStatistics(configuration.iterations, executionTimes);
	csvHandler.AddRowToCsv(row);
	csvHandler.WriteCsv("statistics.csv");

	return 0;
}
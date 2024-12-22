#pragma once

#include <vector>
#include <string>
#include <thread>

std::string trim(const std::string& str);
std::vector<std::string> splitLines(const std::string& input);
std::vector<std::string> splitBySpace(const std::string& input);

//Runs jobs in parallel in threads (or in serial if in a debug build, for easier debugging)
template <typename T, typename Func>
static void parallelExecute(std::vector<T>& jobs, Func func) {
	#ifdef _DEBUG
	for (auto& J : jobs)
		func(J);
	#else
	std::vector<std::thread> threads;
	for (auto& J : jobs)
		threads.emplace_back([JP = &J, func]() { func(*JP); });
	for (auto& t : threads)
		t.join();
	#endif
}

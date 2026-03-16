/*
 * Auto test framework of performance and correctness.
 * Merged from auto_test.cpp (with quantization) and auto_test_woquant.cpp (without quantization).
 *
 * Usage:
 *   ./auto_test --quant [test_case] [range_lo range_hi]   # enable auto quantization
 *   ./auto_test          [test_case] [range_lo range_hi]   # disable auto quantization (default)
 *
 * Run with: `./auto_test [--quant] 2> err.log`
 */

#include <algorithm>
#include <assert.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctype.h>
#include <fstream>
#include <iostream>
#include <math.h>
#include <numeric>
#include <sstream>
#include <vector>
#include <tuple>

const size_t  iteration_num	= 5;
constexpr int WARMUP_ITERATIONS = 1;

/* Global flag: whether auto quantization is enabled */
static bool g_enable_quant = false;

struct perfData {
	int64_t		    inst_count_avg;
	int64_t		    time_consumption_avg;
	int64_t		    ir_lines;
	int64_t		    library_size;
	std::vector<double> function_results;
};

struct timerData {
	int64_t		    inst_count_avg = -1;
	double		    time_consumption_avg;
	double		    compile_time_avg;
	std::vector<double> ms_time_consumption;
	int64_t		    ir_lines;
	int64_t		    library_size;
	std::vector<double> function_results;
	std::vector<double> compile_time;
};

#define TLOG_TIMESPEC_NSEC_PER_SEC 1000000


/***************************************
 * Timer functions of the test framework
 ***************************************/

typedef struct timespec timespec;

timespec
diff(timespec start, timespec end)
{
	timespec temp;
	if ((end.tv_nsec - start.tv_nsec) < 0)
	{
		temp.tv_sec  = end.tv_sec - start.tv_sec - 1;
		temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
	}
	else
	{
		temp.tv_sec  = end.tv_sec - start.tv_sec;
		temp.tv_nsec = end.tv_nsec - start.tv_nsec;
	}
	return temp;
}

timespec
sum(timespec t1, timespec t2)
{
	timespec temp;
	if (t1.tv_nsec + t2.tv_nsec >= 1000000000)
	{
		temp.tv_sec  = t1.tv_sec + t2.tv_sec + 1;
		temp.tv_nsec = t1.tv_nsec + t2.tv_nsec - 1000000000;
	}
	else
	{
		temp.tv_sec  = t1.tv_sec + t2.tv_sec;
		temp.tv_nsec = t1.tv_nsec + t2.tv_nsec;
	}
	return temp;
}

void
printTimeSpec(timespec t, const char * prefix)
{
	printf("%s: %d.%09d\n", prefix, (int)t.tv_sec, (int)t.tv_nsec);
}

timespec
tic()
{
	timespec start_time;
	clock_gettime(CLOCK_REALTIME, &start_time);
	return start_time;
}

timespec
toc(timespec * start_time, const char * prefix, bool print)
{
	timespec current_time;
	clock_gettime(CLOCK_REALTIME, &current_time);
	timespec time_diff = diff(*start_time, current_time);
	if (print)
		printTimeSpec(time_diff, prefix);
	*start_time = current_time;
	return time_diff;
}

/* ---------- trimmed mean helper ---------- */
template <typename T>
double
trimmedMean(std::vector<T> v, double trim_ratio = 0.10)
{
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());

	size_t n   = v.size();
	size_t cut = std::max<size_t>(1, static_cast<size_t>(n * trim_ratio));
	if (cut * 2 >= n)
		cut = n / 2;

	double sum = std::accumulate(v.begin() + cut, v.end() - cut, 0.0);
	return sum / static_cast<double>(n - 2 * cut);
}

std::string
normalizeTestCaseForMake(const std::string & test_case)
{
	if (test_case == "sin_cosf")
		return "perf_sincosf";
	if (test_case == "sin_cosf_opt")
		return "perf_sincosf_opt";
	if (test_case.rfind("e_", 0) == 0)
		return "perf_" + test_case.substr(2);
	return test_case;
}

double
compileTargetCode(const std::string & test_case)
{
	timespec    compile_timer = tic();
	std::string make_target	  = normalizeTestCaseForMake(test_case);
	std::string cmd		  = "bash -c 'make " + make_target + " >& compile.log'";
	int	    ret		  = system(cmd.c_str());
	timespec    compile_time  = toc(&compile_timer, "compile time", false);
	if (ret != 0)
		exit(-1);
	return (double)compile_time.tv_sec + (double)compile_time.tv_nsec / TLOG_TIMESPEC_NSEC_PER_SEC;
}

/*
 * Get number from:
 *  36,200,478      instructions
 *  0.013535825 seconds time elapsed
 */
int64_t
getPerfCount(const std::string & string, size_t position)
{
	std::string substring;
	substring = string.substr(0, position);
	substring.erase(
	    std::remove_if(substring.begin(),
			   substring.end(),
			   static_cast<int (*)(int)>(&ispunct)),
	    substring.end());
	substring.erase(
	    std::remove_if(substring.begin(),
			   substring.end(),
			   static_cast<int (*)(int)>(&isspace)),
	    substring.end());
	return std::stoi(substring);
}

/*
 * Get number from:
 *  computation delay: 0.001342399
 */
double
getTimerConsumption(const std::string & string, size_t position)
{
	std::string substring;
	substring = string.substr(position, string.size());
	return std::stod(substring);
}

/*
 * Get number from:
 *  results: 0.517104	0.809373	0.043233	-0.805564	-0.973201
 */
std::vector<double>
getFunctionResults(const std::string & string, size_t position)
{
	std::vector<double> res;
	std::stringstream   ss;
	std::string	    tmp;
	ss << string;
	double number;
	while (!ss.eof())
	{
		ss >> tmp;
		if (std::stringstream(tmp) >> number)
			res.emplace_back(number);
	}
	return res;
}

std::pair<int64_t, int64_t>
processDataPerf(const std::string test_case, const std::string params)
{
	std::string line;
	size_t	    position;
	int64_t	    inst_count, time_consumption;
	std::string make_target = normalizeTestCaseForMake(test_case);

	// compile: add AUTO_QUANT=1 prefix for _opt targets when quantization is enabled
	std::string quant_prefix = "";
	if (g_enable_quant && test_case.find("_opt") != std::string::npos)
	{
		quant_prefix = "AUTO_QUANT=1 ";
	}
	std::string cmd = "bash -c '" + quant_prefix + "make " + make_target + " >& compile.log'";
	std::cout << "[DEBUG] Running command: " << cmd << std::endl;
	int command_return = system(cmd.c_str());
	if (command_return != 0)
	{
		return std::make_pair(0, 0);
	}
	cmd.clear();
	cmd = "bash -c 'perf stat -B ./main_out " + params;

	cmd += "if=/dev/zero of=/dev/null count=1000000";
	cmd += " 2>&1 | tee tmp.log'";
	command_return = system(cmd.c_str());
	if (command_return != 0)
	{
		return std::make_pair(0, 0);
	}
	std::ifstream ifs("tmp.log");
	if (!ifs.is_open())
	{
		std::cout << "error opening tmp.log";
		assert(false);
	}

	// process
	while (getline(ifs, line))
	{
		position = line.find("instructions");
		if (position != std::string::npos)
		{
			inst_count = getPerfCount(line, position);
		}
		position = line.find("seconds time elapsed");
		if (position != std::string::npos)
		{
			time_consumption = getPerfCount(line, position);
			continue;
		}
	}

	ifs.close();

	return std::make_pair(inst_count, time_consumption);
}

std::pair<double, std::vector<double>>
processDataTimer(const std::string test_case, const std::string params)
{
	std::string	    line;
	size_t		    position;
	double		    time_consumption;
	std::vector<double> function_results;

	/* ---------- Warm-up phase ---------- */
	for (int i = 0; i < WARMUP_ITERATIONS; ++i)
	{
		std::string warm_cmd = "bash -c './main_out " + params + " > /dev/null 2>&1'";
		int	    warm_ret = system(warm_cmd.c_str());
		if (warm_ret != 0)
		{
			std::cerr << "[Warm-up] iteration " << i
				  << " failed for " << test_case
				  << " with params: " << params << std::endl;
		}
	}
	/* ---------- End Warm-up ---------- */

	if (g_enable_quant)
	{
		std::string make_target = normalizeTestCaseForMake(test_case);
		// When quantization is enabled, compile inside processDataTimer with AUTO_QUANT=1 for _opt targets
		std::string quant_prefix = (test_case.find("_opt") != std::string::npos) ? "AUTO_QUANT=1 " : "";
		std::string cmd		 = "bash -c '" + quant_prefix + " make " + make_target + " >& compile.log'";
		std::cout << "[DEBUG] Running command: " << cmd << std::endl;
		int command_return = system(cmd.c_str());
		if (command_return != 0)
		{
			return std::make_pair(0, std::vector<double>(0));
		}
	}

	// run
	std::string cmd = "bash -c './main_out " + params;
	cmd += " 2>&1 | tee tmp.log'";
	int command_return = system(cmd.c_str());
	if (command_return != 0)
	{
		return std::make_pair(0, std::vector<double>(0));
	}
	std::ifstream ifs("tmp.log");
	if (!ifs.is_open())
	{
		std::cout << "error opening tmp.log";
		assert(false);
	}

	// process
	while (getline(ifs, line))
	{
		std::string key = "computation delay: ";
		position	= line.find(key);
		if (position != std::string::npos)
		{
			time_consumption = getTimerConsumption(line, position + key.size());
		}
		key	 = "results: ";
		position = line.find(key);
		if (position != std::string::npos)
		{
			function_results = getFunctionResults(line, position + key.size());
		}
	}

	ifs.close();

	return std::make_pair(time_consumption, function_results);
}

std::string
change_nt_range(const std::string & cmd1, const std::string & cmd2, const std::vector<double> & params)
{
	std::string param_str;
	std::string change_nt_cmd;
	param_str.clear();
	change_nt_cmd.clear();
	change_nt_cmd = cmd1;
	// prepare parameters
	for (const auto & pp : params)
	{
		param_str += std::to_string(pp) + " ";
		change_nt_cmd += std::to_string(pp) + " mjf, ";
	}

	change_nt_cmd.erase(change_nt_cmd.end() - 2);
	change_nt_cmd += cmd2;
	change_nt_cmd = "bash -c \"" + change_nt_cmd + "\"";
	system(change_nt_cmd.c_str());

	return param_str;
}

int64_t
exactNumber()
{
	std::ifstream ifs("tmp.log");
	if (!ifs.is_open())
	{
		std::cout << "error opening tmp.log";
		assert(false);
	}

	// process
	std::string line;
	std::getline(ifs, line);
	auto it = std::remove_if(line.begin(), line.end(), [](const char & c) {
		return !std::isdigit(c);
	});

	line.erase(it, line.end());

	ifs.close();

	char * pEnd;

	return std::strtol(line.c_str(), &pEnd, 10);
}

int64_t
getIrLines()
{
	std::string cmd		   = "bash -c 'wc -l out.ll >& tmp.log'";
	int	    command_return = system(cmd.c_str());
	if (command_return != 0)
		return 0;

	return exactNumber();
}

int64_t
getLibSize()
{
	std::string cmd		   = "bash -c 'wc -c libout.a >& tmp.log'";
	int	    command_return = system(cmd.c_str());
	if (command_return != 0)
		return 0;

	return exactNumber();
}

struct perfData
recordData(const std::string & test_cases, const std::string & param_str, std::ofstream & ofs)
{
	perfData perf_data = {0, 0, 0, 0};

	for (size_t idx = 0; idx < iteration_num; idx++)
	{
		const std::pair<int64_t, int64_t> inst_time_data = processDataPerf(test_cases, param_str);
		perf_data.inst_count_avg += (inst_time_data.first / 1000);
		perf_data.time_consumption_avg += (inst_time_data.second / 1000);
	}
	perf_data.inst_count_avg /= iteration_num;
	perf_data.time_consumption_avg /= iteration_num;

	// check library size
	perf_data.ir_lines     = getIrLines();
	perf_data.library_size = getLibSize();

	ofs << test_cases << "\t" << param_str << "\t" << perf_data.inst_count_avg
	    << "\t" << perf_data.time_consumption_avg << "\t" << perf_data.ir_lines << "\t" << perf_data.library_size << std::endl;

	return perf_data;
}

struct timerData
recordTimerData(const std::string & test_cases, const std::string & param_str, int precision_bits, std::ofstream & ofs)
{
	timerData timer_data;
	for (size_t idx = 0; idx < iteration_num; idx++)
	{
		double compile_time = compileTargetCode(test_cases);
		timer_data.compile_time.emplace_back(compile_time);
		const std::pair<double, std::vector<double>> data_timer_res = processDataTimer(test_cases, param_str);
		timer_data.ms_time_consumption.emplace_back(data_timer_res.first);
		std::copy_if(data_timer_res.second.begin(), data_timer_res.second.end(),
			     std::back_inserter(timer_data.function_results),
			     [test_cases, param_str, timer_data, data_timer_res](double val) {
				     if (!timer_data.function_results.empty())
				     {
					     if (!std::equal(timer_data.function_results.begin(), timer_data.function_results.end(),
							     data_timer_res.second.begin()))
						     std::cerr << "result error within iteration: " << test_cases << " with parameters: " << param_str << std::endl;
					     return false;
				     }
				     else
					     return true;
			     });
	}
	// check library size
	timer_data.ir_lines	= getIrLines();
	timer_data.library_size = getLibSize();

	/* ---------- runtime trimmed-mean ---------- */
	timer_data.time_consumption_avg = trimmedMean(timer_data.ms_time_consumption);
	/* ------------------------------------------ */

	if (g_enable_quant)
	{
		// quant mode: log with precision_bits column
		ofs << test_cases << "\t" << param_str << "\t" << precision_bits << "\t"
		    << timer_data.inst_count_avg << "\t"
		    << std::accumulate(timer_data.ms_time_consumption.begin(), timer_data.ms_time_consumption.end(), 0.0) / timer_data.ms_time_consumption.size() << "\t"
		    << timer_data.ir_lines << "\t" << timer_data.library_size << "\t"
		    << std::accumulate(timer_data.compile_time.begin(), timer_data.compile_time.end(), 0.0) / timer_data.compile_time.size()
		    << std::endl;
	}
	else
	{
		// no-quant mode: log without precision_bits column
		ofs << test_cases << "\t" << param_str << "\t" << timer_data.inst_count_avg
		    << "\t" << timer_data.time_consumption_avg
		    << "\t" << timer_data.ir_lines << "\t" << timer_data.library_size
		    << "\t" << std::accumulate(timer_data.compile_time.begin(), timer_data.compile_time.end(), 0.0) / timer_data.compile_time.size()
		    << std::endl;
	}

	return timer_data;
}

static void
printUsage(const char * prog)
{
	std::cout << "Usage: " << prog << " [--quant] [test_case] [range_lo range_hi]" << std::endl;
	std::cout << "  --quant    Enable auto quantization mode" << std::endl;
	std::cout << "  test_case  Specific test case to run (e.g. perf_j0)" << std::endl;
	std::cout << "  range_lo range_hi  Custom parameter range" << std::endl;
}

int
main(int argc, char ** argv)
{
	/*
	 * Parse arguments:
	 *   --quant flag can appear anywhere, remaining positional args are:
	 *     [test_case] [range_lo range_hi]
	 */
	std::vector<std::string> positional_args;
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--quant") == 0)
		{
			g_enable_quant = true;
		}
		else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			printUsage(argv[0]);
			return 0;
		}
		else
		{
			positional_args.emplace_back(argv[i]);
		}
	}

	std::cout << "[INFO] Auto quantization: " << (g_enable_quant ? "ENABLED" : "DISABLED") << std::endl;

	/* ---------- Default test cases ---------- */
	std::vector<std::string> test_cases;
	if (g_enable_quant)
	{
		test_cases = {"e_y0", "e_j0", "e_exp", "e_log", "sin_cosf"};
	}
	else
	{
		test_cases = {
		    "perf_exp", "perf_log",
		    "perf_acosh", "perf_j0",
		    "perf_y0", "perf_rem_pio2", "perf_sincosf",
		    "perf_float64_add", "perf_float64_div",
		    "perf_float64_mul"};
	}

	/* Override test case if provided as first positional arg */
	if (positional_args.size() >= 1)
	{
		test_cases.clear();
		test_cases.emplace_back(positional_args[0]);
	}

	/* ---------- Output files ---------- */
	std::string   log_suffix = g_enable_quant ? "quant" : "woquant";
	std::ofstream ofs("perf_" + log_suffix + ".log");
	if (!ofs.is_open())
	{
		std::cout << "error opening perf_" << log_suffix << ".log";
		return -1;
	}

	std::ofstream avg_speedup("average_speedup_" + log_suffix + ".log");
	if (!avg_speedup.is_open())
	{
		std::cout << "error opening average_speedup_" << log_suffix << ".log";
		return -1;
	}

	/* ---------- Parameters ---------- */
	std::vector<std::tuple<std::vector<double>, int, int>> normalParameters;

	if (g_enable_quant)
	{
		normalParameters = {
		    // BMX055 acceleration
		    {{-2, 2}, 12, 9},
		    {{-4, 4}, 12, 8},
		    {{-8, 8}, 12, 7},
		    {{-16, 16}, 12, 6},
		    // BMX055 gyroscope
		    {{-125, 125}, 16, 8},
		    // LM35 Centigrade Temperature Sensor
		    {{-40, 110}, 10, 2},
		    {{-55, 150}, 11, 3},
		    {{0, 100}, 10, 3},
		    {{0, 70}, 10, 3},
		    // LPS25H Pressure Sensor
		    {{260, 1260}, 14, 4},
		    // MAX31820 1-Wire Ambient Temperature Sensor
		    {{10, 45}, 12, 6},
		    {{-55, 125}, 11, 3},
		    // DHT11 Humidity Sensor
		    {{20, 80}, 8, 2},
		    {{0, 50}, 8, 2},
		    // LMP82064 Current Sensor and Voltage Monitor with SPI
		    {{-0.2, 2}, 14, 12},
		    // PCE-353 LEQ Sound Level Meter
		    {{30, 130}, 8, 1},
		    // LLS05-A Linear Light Sensor
		    {{1, 200}, 10, 2}};
	}
	else
	{
		normalParameters = {
		    // BMX055 acceleration
		    {{-2, 2}, 0, 0},
		    {{-4, 4}, 0, 0},
		    {{-8, 8}, 0, 0},
		    {{-16, 16}, 0, 0},
		    // BMX055 gyroscope
		    {{-125, 125}, 0, 0},
		    // LM35 Centigrade Temperature Sensor
		    {{-40, 110}, 0, 0},
		    {{-55, 150}, 0, 0},
		    {{0, 100}, 0, 0},
		    {{0, 70}, 0, 0},
		    // LPS25H crazyflie
		    {{260, 1260}, 0, 0},
		    // MAX31820 1-Wire Ambient Temperature Sensor
		    {{10, 45}, 0, 0},
		    {{-55, 125}, 0, 0},
		    // DHT11 Humidity Sensor
		    {{20, 80}, 0, 0},
		    {{0, 50}, 0, 0},
		    // LMP82064 Current Sensor and Voltage Monitor with SPI
		    {{-0.2, 2}, 0, 0},
		    // PCE-353 LEQ Sound Level Meter
		    {{30, 130}, 0, 0},
		    // LLS05-A Linear Light Sensor
		    {{1, 200}, 0, 0}};
	}

	std::vector<std::vector<double>> trigonometricParams{
			    {0, 0.17453292519943295},		       // (0, pi/18)
			    {0.6981317007977318, 0.8726646259971648},  // (2pi/9, 5pi/18)
			    {1.3962634015954636, 1.5707963267948966},  // (4pi/9, pi/2)
			    {2.0943951023931953, 2.2689280275926285},  // (2pi/3, 13pi/18)
			    {2.792526803190927, 2.9670597283903604},   // (8pi/9, 17pi/18)
			    {3.490658503988659, 3.665191429188092},    // (10pi/9, 7pi/6)
			    {4.1887902047863905, 4.363323129985824},   // (8pi/6, 25pi/18)
			    {4.886921905584122, 5.061454830783556},    // (14pi/9, 29pi/18)
			    {5.585053606381854, 5.759586531581288},    // (16pi/9, 33pi/18)
			    {5.934119456780721, 6.1086523819801535}    // (17pi/9, 35pi/18)
	};

	/* Override parameters if range is provided as positional args */
	if (positional_args.size() == 3)
	{
		normalParameters.clear();
		std::vector<double> input_param{strtod(positional_args[1].c_str(), nullptr),
						strtod(positional_args[2].c_str(), nullptr)};
		if (g_enable_quant)
		{
			int default_frac_q	   = 8;
			int default_precision_bits = 12;
			normalParameters.emplace_back(std::make_tuple(input_param, default_precision_bits, default_frac_q));
		}
		else
		{
			normalParameters.emplace_back(std::make_tuple(input_param, 0, 0));
		}

		trigonometricParams.clear();
		trigonometricParams.emplace_back(input_param);
	}

	/* ---------- Log headers ---------- */
	if (g_enable_quant)
	{
		ofs << "test case\tparam\tprecision_bits\tinstruction count\ttime consumption\tir lines\tlibrary size\tcompile time" << std::endl;
	}
	else
	{
		ofs << "test case\tparam\tinstruction count\ttime consumption\tir lines\tlibrary size\tcompile time" << std::endl;
	}
	avg_speedup << "test cast\tinstruction count\ttime consumption\tir lines\tlibrary size\tcompile time" << std::endl;

	/* ---------- Main test loop ---------- */
	for (size_t case_id = 0; case_id < test_cases.size(); case_id++)
	{
		int	   avg_inst_speedup	    = 0;
		int	   avg_time_speedup	    = 0;
		int	   avg_compile_time_speedup = 0;
		int	   avg_ir_reduce	    = 0;
		int	   avg_lib_size_reduce	    = 0;
		const bool is_trig		    = test_cases[case_id] == "perf_float64_sin";

		if (!is_trig)
		{
			for (const auto & entry : normalParameters)
			{
				const std::vector<double> & range	   = std::get<0>(entry);
				int			    precision_bits = std::get<1>(entry);
				int			    frac_q	   = std::get<2>(entry);

				/* Quant mode: rebuild Newton with specific MAX_PRECISION_BITS */
				if (g_enable_quant)
				{
					std::string frac_str = std::to_string(frac_q);
					std::string rebuild_cmd =
					    "bash -c '"
					    "rm -f ../../../../src/newton/newton-irPass-LLVMIR-quantization.o ../../../../src/newton/newton-irPass-LLVMIR-optimizeByRange.o && "
					    "make -C ../../../../src/newton MAX_PRECISION_BITS=" +
					    frac_str + " BIT_WIDTH=32 > build_" + frac_str + ".log 2>&1'";

					std::cout << "[INFO] Rebuilding Newton with MAX_PRECISION_BITS = " << frac_q << std::endl;
					int ret = system(rebuild_cmd.c_str());
					if (ret != 0)
					{
						std::cerr << "[ERROR] Failed to rebuild Newton with MAX_PRECISION_BITS=" << frac_q << std::endl;
						continue;
					}
				}

				const std::string param_str = change_nt_range("sed -i 's/3 mjf, 10 mjf/", "/g' ../../sensors/test.nt", range);
				const double	  p1	    = range.front() + 0.6;
				const double	  p2	    = range.back() + 0.3;
				change_nt_range("sed -i 's/15 mjf, 36 mjf/", "/g' ../../sensors/test.nt", {p1, p2});

				timerData ori_perf_data = recordTimerData(test_cases[case_id], param_str, precision_bits, ofs);
				timerData opt_perf_data = recordTimerData(test_cases[case_id] + "_opt", param_str, precision_bits, ofs);

				// check function results
				if (!std::equal(ori_perf_data.function_results.begin(), ori_perf_data.function_results.end(),
						opt_perf_data.function_results.begin()))
				{
					std::cerr << "result error: " << test_cases[case_id] << " with parameters: " << param_str
						  << "ori: " << ori_perf_data.function_results[0] << ", opt: " << opt_perf_data.function_results[0] << std::endl;
				}

				// remove element if ori < opt
				assert(ori_perf_data.ms_time_consumption.size() == opt_perf_data.ms_time_consumption.size());
				auto itOri = ori_perf_data.ms_time_consumption.begin();
				for (auto itOpt = opt_perf_data.ms_time_consumption.begin();
				     itOpt != opt_perf_data.ms_time_consumption.end();)
				{
					if (*itOri < *itOpt)
					{
						itOri = ori_perf_data.ms_time_consumption.erase(itOri);
						itOpt = opt_perf_data.ms_time_consumption.erase(itOpt);
					}
					else
					{
						itOri++;
						itOpt++;
					}
				}

				int inst_speedup, time_speedup, ir_reduce, lib_size_reduce, compile_time_speedup;
				if (ori_perf_data.ms_time_consumption.empty())
				{
					assert(opt_perf_data.ms_time_consumption.empty() && "erase mis-match!");
					inst_speedup = 0;
					time_speedup = 0;
				}
				else
				{
					ori_perf_data.time_consumption_avg = trimmedMean(ori_perf_data.ms_time_consumption);
					opt_perf_data.time_consumption_avg = trimmedMean(opt_perf_data.ms_time_consumption);

					ori_perf_data.compile_time_avg = std::accumulate(ori_perf_data.compile_time.begin(),
											 ori_perf_data.compile_time.end(),
											 0.0) /
									 ori_perf_data.compile_time.size();
					opt_perf_data.compile_time_avg = std::accumulate(opt_perf_data.compile_time.begin(),
											 opt_perf_data.compile_time.end(),
											 0.0) /
									 opt_perf_data.compile_time.size();

					inst_speedup	     = round((ori_perf_data.inst_count_avg - opt_perf_data.inst_count_avg) * 100 / opt_perf_data.inst_count_avg);
					time_speedup	     = round((ori_perf_data.time_consumption_avg - opt_perf_data.time_consumption_avg) * 100 / opt_perf_data.time_consumption_avg);
					compile_time_speedup = round((ori_perf_data.compile_time_avg - opt_perf_data.compile_time_avg) * 100 / opt_perf_data.compile_time_avg);
				}

				ir_reduce	= round((ori_perf_data.ir_lines - opt_perf_data.ir_lines) * 100.0 / opt_perf_data.ir_lines);
				lib_size_reduce = round((ori_perf_data.library_size - opt_perf_data.library_size) * 100.0 / opt_perf_data.library_size);

				ofs << "speed up after optimization\t" << param_str << "\t" << inst_speedup << "%\t" << time_speedup << "%\t"
				    << ir_reduce << "%\t" << lib_size_reduce << "%\t" << compile_time_speedup << "%" << std::endl;
				std::cout << test_cases[case_id] << ": speed up after optimization\t" << param_str << "\t" << inst_speedup
					  << "%\t" << time_speedup << "%\t"
					  << ir_reduce << "%\t" << lib_size_reduce << "%\t" << compile_time_speedup << "%" << std::endl;

				avg_inst_speedup += inst_speedup;
				avg_time_speedup += time_speedup;
				avg_ir_reduce += ir_reduce;
				avg_lib_size_reduce += lib_size_reduce;
				avg_compile_time_speedup += compile_time_speedup;

				// reset test.nt
				change_nt_range("sed -i 's/", "/3 mjf, 10 mjf/g' ../../sensors/test.nt",
						{range.front(), range.back()});
				change_nt_range("sed -i 's/", "/15 mjf, 36 mjf/g' ../../sensors/test.nt", {p1, p2});
			}

			size_t count		 = is_trig ? trigonometricParams.size() : normalParameters.size();
			avg_inst_speedup	 = round(avg_inst_speedup / count);
			avg_time_speedup	 = round(avg_time_speedup / count);
			avg_ir_reduce		 = round(avg_ir_reduce / count);
			avg_lib_size_reduce	 = round(avg_lib_size_reduce / count);
			avg_compile_time_speedup = round(avg_compile_time_speedup / count);
			avg_speedup << test_cases[case_id] << "\t" << avg_inst_speedup << "%\t"
				    << avg_time_speedup << "%\t" << avg_ir_reduce << "%\t" << avg_lib_size_reduce << "%\t"
				    << avg_compile_time_speedup << "%" << std::endl;

				if (test_cases[case_id] == "perf_float64_sin")
				{
					// trigonometricParams cannot have extention
					break;
				}
			}

			//		ofs.close();
			//
			//		return 0;
		}
	}

	ofs.close();
	return 0;
}

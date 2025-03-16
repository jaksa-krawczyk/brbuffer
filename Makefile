tests: stability.o perf_throughput.o perf_cycles.o
	g++ -o stability stability.o
	g++ -o perf_throughput perf_throughput.o
	g++ -o perf_cycles perf_cycles.o

stability.o:
	g++ tests/stability.cpp -c -O2 -pthread -I$(CURDIR) --std=c++20

perf_throughput.o :
	g++ tests/perf_throughput.cpp -c -O2 -pthread -I$(CURDIR) --std=c++20

perf_cycles.o :
	g++ tests/perf_cycles.cpp -c -O2 -pthread -I$(CURDIR) --std=c++20

clean:
	rm stability stability.o perf_throughput perf_throughput.o perf_cycles perf_cycles.o

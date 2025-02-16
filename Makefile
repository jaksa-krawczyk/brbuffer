tests: tests.o
	g++ -o stability stability.o

tests.o:
	g++ tests/stability.cpp -c -O2 -pthread -I$(CURDIR) --std=c++20

clean:
	rm stability stability.o

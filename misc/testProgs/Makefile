# Common deps
DEPS=Makefile

default: test_affinity

test_affinity: $(DEPS) test-affinity.cpp
	g++ -O3 -g -o test_affinity test-affinity.cpp -pthread

run_tests: default
	./test_affinity

clean:
	rm -f *.o *.so *.a test_*

.PHONY: clean default


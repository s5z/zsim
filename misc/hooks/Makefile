#JDK_PATH=../jdk1.5.0_22
#JDK_PATH=/usr/lib/jvm/java-7-oracle
# This should work regardless of the JDK flavor (OpenJDK/Oracle), at least on Ubuntu
JDK_PATH=$(shell dirname $(shell dirname $(shell (readlink -f `which javac`))))

# Common deps
DEPS=Makefile zsim_hooks.h

default: test_c test_cpp test_fortran test.class

libfortran_hooks.a: $(DEPS)
	gcc -O3 -g -fPIC -o fortran_hooks.o -c fortran_hooks.c
	ar rcs libfortran_hooks.a fortran_hooks.o
	ranlib libfortran_hooks.a

zsim.class: $(DEPS) zsim_jni.cpp zsim.java
	$(JDK_PATH)/bin/javah -o zsim_jni.h zsim  # generates header from zsim.java
	g++ -O3 -g -std=c++0x -shared -fPIC -o libzsim_jni.so zsim_jni.cpp -I$(JDK_PATH)/include -I$(JDK_PATH)/include/linux
	$(JDK_PATH)/bin/javac zsim.java
	#$(JDK_PATH)/bin/jar cf zsim.jar zsim.class # not needed

test_c: $(DEPS) test.c
	gcc -O3 -g -o test_c test.c

test_cpp: $(DEPS) test.cpp
	g++ -O3 -g -o test_cpp test.cpp

test_fortran: $(DEPS) test.f libfortran_hooks.a
	gfortran -o test_fortran test.f -L. -lfortran_hooks

test.class: $(DEPS) test.java zsim.class
	$(JDK_PATH)/bin/javac test.java

run_tests: test_c test_cpp test_fortran test.class
	./test_c
	./test_cpp
	./test_fortran
	java -Djava.library.path=. test

clean:
	rm -f *.o *.so *.a *.jar *.class test_* zsim_jni.h

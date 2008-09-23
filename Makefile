
LIBRARIES+=-lboost_thread-mt
LIBRARIES+=-lboost_program_options

preempt-test: preempt-test.cc
	g++ preempt-test.cc -g $(LIBRARIES) -o preempt-test
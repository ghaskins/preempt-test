// Preemption-test - v4
//
// Copyright (c) 2007, Novell
//
// Author: Gregory Haskins <ghaskins@novell.com>
//
// This will test several attributes of the linux RT scheduler.  It checks to
// make sure that the scheduler allows higher priority tasks to preempt lower
// ones, and it measures the latency it takes to do so.
//
// The basic premise of this test is to synchronously launch a series of
// threads which have a monotonically increasing RT priority by a parent
// that is running at the highest priority.  So for 5 threads, we have 5
// worker threads at priorities 1-5, and a parent at 6.  The sequence would
// basically go: 6-1-6-2-6-3-6-4-6-5. We calibrate a busy-loop so that each
// child gets (by default) 20ms worth of busy work to do. We then measure the
// latency to wake the child, wake the parent, and run the entire test.
//
// The expected result is that we should see short wake-latencies across the
// board, and the higher priority threads should finish their work first.
//
// This test was inspired by Steven Rostedt's "rt-migration-test", so many
// thanks to him for getting this effort off the ground.
//
// Preemption-test is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License v2 
// as published by the Free Software Foundation.
// 
// Preemption-test is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with Preemption-test; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA


// compile with: g++ preempt.cc -g -lboost_thread-mt -lboost_program_options


#include <iostream>
#include <sstream>
#include <vector>
#include <list>

#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include <boost/version.hpp>
#include <boost/thread/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/array.hpp>
#include <boost/program_options.hpp>

#if BOOST_VERSION >= 103600

#include <boost/thread/locks.hpp>

typedef boost::mutex                                     Mutex;
typedef boost::condition_variable                        CondVar;
typedef boost::unique_lock<boost::mutex>                 Lock;

#else

#include <boost/thread/detail/lock.hpp>

typedef boost::mutex                                     Mutex;
typedef boost::condition                                 CondVar;
typedef boost::detail::thread::scoped_lock<boost::mutex> Lock;

#endif

#define nano2sec(nan) ((nan) / 1000000000ULL)
#define nano2ms(nan) ((nan) / 1000000ULL)
#define nano2usec(nan) ((nan) / 1000ULL)
#define usec2nano(sec) ((sec) * 1000ULL)
#define ms2nano(ms) ((ms) * 1000000ULL)
#define sec2nano(sec) ((sec) * 1000000000ULL)

typedef unsigned long long u64;
bool affine(false);

//---------------------------------------------------------------------
// Timestamp
//---------------------------------------------------------------------

class Timestamp
{
	friend Timestamp operator-(const Timestamp &lhs, const Timestamp &rhs);
	friend u64       operator/(u64 num, const Timestamp &div);
	friend std::ostream& operator<<(std::ostream &os, const Timestamp &);
	
 public:
	Timestamp() {
		struct timeval tv;

		gettimeofday(&tv, NULL);

		m_time  = sec2nano(tv.tv_sec);
		m_time += usec2nano(tv.tv_usec);
	}
	Timestamp(u64 t) : m_time(t) {}

	bool operator<(const Timestamp &rhs) {
		return m_time < rhs.m_time;
	}

	u64 operator()() { return m_time; }

 private:
	u64 m_time;
};

Timestamp operator-(const Timestamp &lhs, const Timestamp &rhs)
{
	Timestamp t;

	t.m_time = lhs.m_time - rhs.m_time;

	return t;
}

u64 operator/(u64 num, const Timestamp &div)
{
	return num/div.m_time;
}

std::ostream& operator<<(std::ostream &os, const Timestamp &ts)
{
	os << nano2usec(ts.m_time);
}

//---------------------------------------------------------------------
// Delay
//---------------------------------------------------------------------

class Delay
{
 public:
	Delay() : m_run_interval(0) {}

	Delay(int run_interval) : m_run_interval(run_interval) {}

	void busywait() {
		u64 loops(0);

		for (u64 i(0); i<m_run_interval * 10; i++) {
			Timestamp start;
		
			do {
				loops++;
			}while ((Timestamp()  - start) < 100000);
		}
	}

 private:
	int m_run_interval;
};

Delay delay;
int   nr_cpus(-1);

//---------------------------------------------------------------------
// Thread priority
//---------------------------------------------------------------------

static void set_prio(int prio)
{
	int ret;
	struct sched_param param;

	memset(&param, 0, sizeof(param));
	param.sched_priority = prio,

	ret = sched_setscheduler(0, SCHED_FIFO, &param);
	if (ret < 0)
		throw std::runtime_error("sched_setscheduler failed");
}

//---------------------------------------------------------------------
// Worker
//---------------------------------------------------------------------

class Worker
{
 public:
	Worker(int id) :
		m_id(id),
		m_state(ws_init),
		m_affinity(-1)
		{}

	enum State {
		ws_init,
		ws_barrier,
		ws_ready,
		ws_running
	};

	// This is the worker logic (executed within its own thread)
	void run() {
		try { set_prio(m_id); } catch(...) {}

		if (affine)
			set_affinity();

		wait_for_wakeup();

		delay.busywait();

		m_finish_time = Timestamp();
	}

	// Called by the parent to block until the thread is ready
	void barrier() {
		Lock l(m_mutex);

		while (m_state != ws_barrier)
			m_cv.wait(l);
	}

	// Called by the parent to wake-up the worker
	void wakeup() {
		Lock l(m_mutex);

		m_start_time = Timestamp();
		m_state      = ws_ready;
		m_cv.notify_one();

		while (m_state != ws_running)
			m_cv.wait(l);

		m_return_time = Timestamp();
	}

	int       id()       { return m_id; }
	int       affinity() { return m_affinity; }
	Timestamp started()  { return m_start_time; }
	Timestamp woken()    { return m_wake_time; }
	Timestamp returned() { return m_return_time; }
	Timestamp finished() { return m_finish_time; }

 private:
	// Block until the parent wakes us up
	void wait_for_wakeup() {
		Lock l(m_mutex);

		m_state = ws_barrier;
		m_cv.notify_one();

		while (m_state != ws_ready)
			m_cv.wait(l);

		m_wake_time = Timestamp();
		m_state     = ws_running;
		m_cv.notify_one();
	}

	void set_affinity() {
		int       cpu((m_id-1) % nr_cpus);
		int       ret;
		cpu_set_t cpu_mask;
		
		CPU_ZERO(&cpu_mask);
		if (cpu < 0 || cpu > CPU_SETSIZE) {
			std::ostringstream os;

			os << "Illegal cpu: " << cpu << " on worker " << m_id;
			throw std::runtime_error(os.str());
		}
		
		CPU_SET(cpu, &cpu_mask);

		ret= pthread_setaffinity_np(pthread_self(), 
					    sizeof(cpu_mask), &cpu_mask);
		if (ret != 0)
			throw std::runtime_error("setaffinity_np()");

		m_affinity = cpu;
	}


	int       m_id;
	Mutex     m_mutex;
	CondVar   m_cv;
	State     m_state;
	int       m_affinity;
	Timestamp m_start_time, m_wake_time, m_return_time, m_finish_time;
};

typedef boost::shared_ptr<Worker> WorkerPtr;
typedef std::vector<WorkerPtr> WorkerList;
typedef boost::function<void ()> Functor;

//---------------------------------------------------------------------
// Stats
//---------------------------------------------------------------------

class Average
{
	friend std::ostream& operator<<(std::ostream &os, const Average &);
 public:
	Average() : m_val(0), m_count(0) {}

	void operator+=(u64 newval) {
		m_val = (m_val * m_count) + newval;
		m_count++;

		m_val /= m_count;
	}

	u64 operator()() { return m_val; }

 private:
	u64 m_val;
	u64 m_count;
};

std::ostream& operator<<(std::ostream &os, const Average &a)
{
	os <<  nano2usec(a.m_val);
}


class Stats
{
	friend std::ostream& operator<<(std::ostream &os, const Stats &);
 public:
	Stats() : m_min(0, -1), m_max(0, 0) {}

	typedef std::pair<int, unsigned int> intpair_t;

	void update(int pos, u64 lat) {
		m_ave += lat;
		
		if (m_min.second > lat)
			m_min = intpair_t(pos, lat);

		if (m_max.second < lat)
			m_max = intpair_t(pos, lat);
	}

 private:
	Average      m_ave;
	intpair_t    m_min;
	intpair_t    m_max;
	
};

std::ostream& operator<<(std::ostream &os, const Stats &s)
{
	os << "[min: " <<  nano2usec(s.m_min.second) 
	   << "us @ row " << s.m_min.first << "]"
	   << " [max: " <<  nano2usec(s.m_max.second)
	   << "us @ row " << s.m_max.first << "]"
	   << " {ave: " << s.m_ave << "us]";
}

bool compare_finish(const WorkerPtr &rhs, const WorkerPtr &lhs)
{
	if (rhs->finished() < lhs->finished())
		return true;

	return false;
}

//---------------------------------------------------------------------
// Trace
//---------------------------------------------------------------------

class Tracer
{
 public:
	Tracer() {
		int ret;
		
		if (getuid() != 0)
			throw std::runtime_error("needs to run as root");

		ret = system("cat /proc/sys/kernel/mcount_enabled >/dev/null 2>/dev/null");
		if (ret)
			throw std::runtime_error("CONFIG_LATENCY_TRACING not enabled?");

		system("echo 1 > /proc/sys/kernel/trace_user_triggered");
		system("[ -e /proc/sys/kernel/wakeup_timing ] && echo 0 > /proc/sys/kernel/wakeup_timing");
		system("echo 1 > /proc/sys/kernel/trace_enabled");
		system("echo 1 > /proc/sys/kernel/mcount_enabled");
		system("echo 0 > /proc/sys/kernel/trace_freerunning");
		system("echo 0 > /proc/sys/kernel/trace_print_on_crash");
		system("echo 0 > /proc/sys/kernel/trace_verbose");
		system("echo 0 > /proc/sys/kernel/preempt_thresh 2>/dev/null");
		system("echo 0 > /proc/sys/kernel/preempt_max_latency 2>/dev/null");
		
		// start tracing
		if (prctl(0, 1))
			throw std::runtime_error("couldnt start tracing!");
	}

	~Tracer() {
		if (prctl(0, 0))
			std::cerr << "warning: couldnt stop tracing!"
				  << std::endl;

		system("echo 0 > /proc/sys/kernel/trace_user_triggered");
		system("echo 0 > /proc/sys/kernel/trace_enabled");	
	}
};

typedef boost::shared_ptr<Tracer> TracerPtr;


//---------------------------------------------------------------------
// Logdev
//---------------------------------------------------------------------

class Logdev
{
 public:
	Logdev() {
		int ret;
		
		if (getuid() != 0)
			throw std::runtime_error("needs to run as root");

		ret = system("cat /debugfs/logdev/print >/dev/null 2>/dev/null");
		if (ret)
			throw std::runtime_error("LOGDEV not enabled?");

		system("echo 1 > /debugfs/logdev/print");
	}

	~Logdev() {
		system("echo 0 > /debugfs/logdev/print");
	}
};

typedef boost::shared_ptr<Logdev> LogdevPtr;

//---------------------------------------------------------------------
// Test
//---------------------------------------------------------------------

void run(int nr_workers)
{
	WorkerList          workers(nr_workers);
	boost::thread_group tg;

	// Create nr_worker threads in our thread-group
	for (int i(0); i<nr_workers; i++) {
		WorkerPtr  w(new Worker(i+1));
		Functor    f(boost::bind(boost::mem_fn(&Worker::run), w));

		workers[i] = w;
		tg.create_thread(f);

		w->barrier();
	}

	Timestamp start_time;

	// Now synchronously start each thread, starting with the lowest-pri
	for (int i(0); i<nr_workers; i++)
		workers[i]->wakeup();

	tg.join_all();

	// Resort the list by finish-order (first to last)
	sort(workers.begin(), workers.end(), compare_finish);

	// Display the results
	int   pos(0);
	Stats stats;

	for (WorkerList::iterator iter(workers.begin());
	     iter != workers.end(); ++iter, ++pos) {
		WorkerPtr w(*iter);
		Timestamp wake_lat(w->woken() - w->started());
		Timestamp ret_lat(w->returned() - w->woken());
		Timestamp run_time(w->finished() - w->woken());
		Timestamp finish_ts(w->finished() - start_time);
		Timestamp delta_ts(finish_ts - run_time);

		std::cout << pos << " ->" 
			  << " p:" << w->id()
			  << " a: " << w->affinity()
			  << " l: " << wake_lat << "/" << ret_lat
			  << " t: "    << run_time
			  << "/" << finish_ts
			  << "/" << delta_ts
			  << std::endl;

		stats.update(pos, wake_lat()); 
		stats.update(pos, ret_lat());
	}

	std::cout << std::endl
		  << "--------------------------- " << std::endl
		  << "Stats: " << stats << std::endl
		  << "--------------------------- " << std::endl;

	// Validate the result
	int row(0);
	int errors(0);

	while (pos) {
		int hpos(pos);
		int count;

		for (count=0; count<nr_cpus && pos; count++, pos--);

		for (int i(0); i<count; i++, row++) {
			WorkerPtr w = workers[0];
			workers.erase(workers.begin());

			if ((w->id() > hpos) || (w->id() <= pos)) {
				std::cerr << "Error: row "
					  << row
					  << ": "
					  << w->id()
					  << " != ("
					  << hpos
					  << " >= x > "
					  << pos
					  << ")"
					  << std::endl;

				errors++;
			}
				
		}
	}

	if (!errors)
		std::cout << "Test PASSED" << std::endl;
	else
		std::cout << "--------------------------- " << std::endl
			  << "Test FAILED with "
			  << errors << " errors" << std::endl;
}

namespace po = boost::program_options;

int main(int argc, char **argv)
{
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (nr_cpus < 0) {
		std::cerr << "Could not determine cpu count" << std::endl;
		exit(-1);
	}

	int       nr_workers(98);
	int       run_interval(20); // ms
	TracerPtr tracer;
	LogdevPtr logdev;

	po::options_description desc("Allowed options");
	desc.add_options()
		("help,h", "produces help message")
		("numthreads,t", po::value<int>(&nr_workers),
		 "Number of threads")
		("duration,d", po::value<int>(&run_interval),
		 "Duration per thread (ms)")
		("affine,a", "Affine threads to a single CPU")
		("latency-trace,l", "Enable latency-tracing")
		("logdev", "Enable logdev logging")
		;

	po::variables_map vm;
	po::store(parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	std::cout << "Starting test with " << nr_workers
		  << " threads on " << nr_cpus << " cpus"<< std::endl;

	try {
		set_prio(nr_workers+1);
	} catch (...) {
		std::cerr << "Warning:: Cannot set priority." << std::endl;
	}

	mlockall(MCL_CURRENT | MCL_FUTURE);

	// Calibrate the system to our run-interval
	delay = Delay(run_interval);

	if(vm.count("affine"))
		affine=true;

	if(vm.count("latency-trace"))
		tracer = TracerPtr(new Tracer());

	if(vm.count("logdev"))
		logdev = LogdevPtr(new Logdev());

	run(nr_workers);

	return 0;
}

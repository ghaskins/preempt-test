%define rpmrel _RPM_RELEASE

BuildRequires: boost-devel gcc-c++

Summary: preempt-test: verify proper behavior of the RT scheduler
Name: preempt-test
Version: _RPM_VERSION
License: GPL
Release: %{rpmrel}
Requires: boost
Group: System
Source: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-root

%description
This will test several attributes of the linux RT scheduler.  It checks to
make sure that the scheduler allows higher priority tasks to preempt lower
ones, and it measures the latency it takes to do so.

The basic premise of this test is to synchronously launch a series of
threads which have a monotonically increasing RT priority by a parent
that is running at the highest priority.  So for 5 threads, we have 5
worker threads at priorities 1-5, and a parent at 6.  The sequence would
basically go: 6-1-6-2-6-3-6-4-6-5. We calibrate a busy-loop so that each
child gets (by default) 20ms worth of busy work to do. We then measure the
latency to wake the child, wake the parent, and run the entire test.

The expected result is that we should see short wake-latencies across the
board, and the higher priority threads should finish their work first.

This test was inspired by Steven Rostedt's "rt-migration-test", so many
thanks to him for getting this effort off the ground.

Authors
--------------------------
  Gregory Haskins <ghaskins@novell.com>

%debug_package
%prep
%setup

%build
make

%install
make install PREFIX=$RPM_BUILD_ROOT

%clean
make clean

%files
%defattr(-,root,root)
/bin/preempt-test

%changelog

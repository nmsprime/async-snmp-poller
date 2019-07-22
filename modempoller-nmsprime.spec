Name: modempoller-nmsprime
Version: 0.0.1
Release: 1
Summary: A highly efficient modem snmp poller

Group: Applications/Communications
License: GPLv3
URL: https://github.com/nmsprime/async-snmp-poller
Source: https://raw.githubusercontent.com/nmsprime/async-snmp-poller/master/src/%{name}.c

BuildRequires: gcc net-snmp-devel mysql-devel

%description
This asynchronous snmp poller solves the issue with Cacti when monitoring several
thousand devices. Cacti produces a massive CPU load and takes a long time to collect
its monitoring data by using hundreds of concurrent php workers which handle each a
small batch of devices sequentially. This blocks the CPU and scales pretty poorly.

%build
gcc -s -l netsnmp $(mysql_config --cflags --libs) -o %{name} %{_sourcedir}/%{name}.c

%install
install -Dm755 %{name} %{buildroot}%{_bindir}/%{name}

%files
%{_bindir}/%{name}

%changelog
* Mon May 06 2019 Ole Ernst <ole.ernst@nmsprime.com> - 0.0.1-1
- Initial RPM release

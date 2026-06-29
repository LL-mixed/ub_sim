Name: linqu-mem-service
Version: %{_mem_service_version}
Release: %{_mem_service_release}
Summary: Lingqu mem_service independent daemon and SDK
License: Apache-2.0
Source0: %{name}-%{version}.tar.gz
%global debug_package %{nil}

%description
Lingqu mem_service daemon, client SDK, examples, release contracts, and deploy
manifests for independent LLM serving and pretraining integration.

%prep
mkdir -p %{_builddir}/%{name}-%{version}
tar -xzf %{SOURCE0} -C %{_builddir}/%{name}-%{version}

%build

%install
mkdir -p %{buildroot}
cp -a %{_builddir}/%{name}-%{version}/usr %{buildroot}/
cp -a %{_builddir}/%{name}-%{version}/etc %{buildroot}/

%files
/usr/bin/linqu_mem_service
/usr/libexec/lingqu/mem_service/linqu_mem_service_host
/usr/include/lingqu/mem_service
/usr/src/lingqu/mem_service
/usr/share/lingqu/mem_service
/usr/lib/systemd/system/linqu_mem_service.service
/usr/lib/systemd/system/linqu_mem_service.host.service
%config(noreplace) /etc/lingqu/mem_service/mem_service.conf

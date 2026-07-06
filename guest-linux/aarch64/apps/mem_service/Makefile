ROOT := ../..
CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
HOST_CC ?= cc
HOST_CFLAGS ?= -O2 -Wall -Wextra
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
INCLUDES := -I$(ROOT) -I$(ROOT)/.. -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue

MEM_SERVICE := $(ROOT)/components/mem_service/mem_service_module.c
MEM_SERVICE_CLUSTER_UTILS := $(ROOT)/components/mem_service/mem_service_cluster_utils.c
MEM_SERVICE_CLUSTER_PAYLOAD := $(ROOT)/components/mem_service/mem_service_cluster_payload.c
MEM_SERVICE_CLUSTER_READ := $(ROOT)/components/mem_service/mem_service_cluster_read.c
MEM_SERVICE_CLUSTER_RUNTIME := $(ROOT)/components/mem_service/mem_service_cluster_runtime.c
MEM_SERVICE_CLUSTER_QUEUE := $(ROOT)/components/mem_service/mem_service_cluster_queue.c
MEM_SERVICE_CLUSTER_OBSERVE := $(ROOT)/components/mem_service/mem_service_cluster_observe.c
MEM_SERVICE_OBMM_OBJECT_FLOW := $(ROOT)/components/mem_service/mem_service_obmm_object_flow.c
MEM_SERVICE_METADATA := $(ROOT)/components/mem_service/mem_service_metadata.c
MEM_SERVICE_DAEMON := $(ROOT)/components/mem_service/mem_service_daemon.c
MEM_SERVICE_CLIENT := $(ROOT)/components/mem_service/mem_service_client.c
MEM_SERVICE_WIRE_CLIENT := $(ROOT)/components/mem_service/mem_service_wire_client.c
MEM_SERVICE_KEYS := $(ROOT)/components/mem_service/mem_service_keys.c
MEM_SERVICE_OBJECT_REFS := $(ROOT)/components/mem_service/mem_service_object_refs.c
MEM_SERVICE_OBMM_OBJECTS := $(ROOT)/components/mem_service/mem_service_obmm_objects.c
MEM_SERVICE_UB_SSD_GSVA_BACKEND := $(ROOT)/components/mem_service/mem_service_ub_ssd_gsva_backend.c
MEM_SERVICE_UB_SSD_GSVA_IO := $(ROOT)/components/mem_service/mem_service_ub_ssd_gsva_io.c
MEM_SERVICE_RECORDS := $(ROOT)/components/mem_service/mem_service_records.c
MEM_SERVICE_QWEN3_RECORDS := $(ROOT)/components/mem_service/mem_service_qwen3_records.c
MEM_SERVICE_QWEN3_RUNTIME := $(ROOT)/components/mem_service/mem_service_qwen3_runtime.c
MEM_SERVICE_QWEN3_DECODE_BARRIER := $(ROOT)/components/mem_service/mem_service_qwen3_decode_barrier.c
MEM_SERVICE_QWEN3_KV_STATE_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_kv_state_flow.c
MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_terminal_token_flow.c
MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c
MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c
MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_engram_publish_flow.c
MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_engram_wait_flow.c
MEM_SERVICE_QWEN3 := $(ROOT)/components/mem_service/mem_service_qwen3.c
LLM_INFER := $(ROOT)/components/llm_infer/llm_infer.c
MEM_SERVICE_RELEASE_MANIFEST := release-manifest.txt
MEM_SERVICE_PACKAGE_MANIFEST := package-manifest.txt
MEM_SERVICE_WIRE_SCHEMA_MANIFEST := wire-schema.txt
MEM_SERVICE_ADMIN_OUTPUT_SCHEMA := admin-output-schema.txt
MEM_SERVICE_UPGRADE_ROLLBACK_POLICY := upgrade-rollback-policy.txt
MEM_SERVICE_OPS_CERTIFICATION_POLICY := ops-certification-policy.txt
MEM_SERVICE_API_ABI_POLICY := api-abi-policy.txt
MEM_SERVICE_COMPAT_MATRIX := compat-matrix.txt
MEM_SERVICE_COMPAT_BASELINE_V1 := compat-baseline-v1.txt
MEM_SERVICE_COMPAT_OLD_NEW_MATRIX := compat-old-new-matrix.txt
MEM_SERVICE_CONFIG_SCHEMA := configs/mem_service.conf.schema
MEM_SERVICE_CONFIG_EXAMPLE := configs/mem_service.example.conf
MEM_SERVICE_CONFIG_RUNTIME := configs/mem_service.runtime.conf
MEM_SERVICE_CONFIG_HOST_RUNTIME := configs/mem_service.host.runtime.conf
MEM_SERVICE_DEPLOY_MANIFEST := deploy/linqu_mem_service.service
MEM_SERVICE_HOST_DEPLOY_MANIFEST := deploy/linqu_mem_service.host.service
MEM_SERVICE_ALERT_RULES := deploy/linqu_mem_service.prometheus-alerts.yml
MEM_SERVICE_RELEASE_SCRIPTS := \
	$(ROOT)/scripts/verify_mem_service_installed_layout.sh \
	$(ROOT)/scripts/verify_mem_service_installed_sdk.sh \
	$(ROOT)/scripts/run_mem_service_linux_ops_ci.sh \
	$(ROOT)/scripts/verify_mem_service_linux_ops_evidence.sh \
	$(ROOT)/scripts/verify_mem_service_ops_certification_bundle.sh \
	$(ROOT)/scripts/run_mem_service_remote_transport_ci.sh \
	$(ROOT)/scripts/verify_mem_service_remote_transport_evidence.sh \
	$(ROOT)/scripts/verify_mem_service_remote_transport_bundle.sh \
	$(ROOT)/scripts/verify_mem_service_release_certification.sh \
	$(ROOT)/scripts/run_mem_service_release_certification_ci.sh
MEM_SERVICE_PACKAGE_TARBALL_NAME := linqu_mem_service-installed-layout-v1.tar
MEM_SERVICE_PKGCONFIG_NAME := lingqu-mem-service.pc
MEM_SERVICE_DEB_NAME := linqu-mem-service
MEM_SERVICE_DEB_VERSION ?= 0.1.0-1
MEM_SERVICE_DEB_ARCH ?= arm64
MEM_SERVICE_RPM_NAME := linqu-mem-service
MEM_SERVICE_RPM_VERSION ?= 0.1.0
MEM_SERVICE_RPM_RELEASE ?= 1
MEM_SERVICE_RPM_ARCH ?= aarch64
MEM_SERVICE_RPM_SPEC := packaging/linqu-mem-service.spec

PREFIX ?= /usr/local
DESTDIR ?=
SYSCONFDIR ?= /etc
SYSTEMDUNITDIR ?= /usr/lib/systemd/system
PACKAGE_PREFIX ?= /usr
PACKAGE_OUT_DIR ?= ../../../../out/mem_service
PACKAGE_STAGE_ROOT := $(PACKAGE_OUT_DIR)/linqu_mem_service.installed-layout-v1.root
PACKAGE_VERIFY_ROOT := $(PACKAGE_OUT_DIR)/linqu_mem_service.installed-layout-v1.verify
PACKAGE_TARBALL := $(PACKAGE_OUT_DIR)/$(MEM_SERVICE_PACKAGE_TARBALL_NAME)
PACKAGE_TARBALL_LIST := $(PACKAGE_OUT_DIR)/$(MEM_SERVICE_PACKAGE_TARBALL_NAME).list
DEB_CC ?= aarch64-linux-gnu-gcc
DEB_HOST_CC ?= $(DEB_CC)
DEB_BUILD_ROOT := $(PACKAGE_OUT_DIR)/linqu_mem_service.deb.build
DEB_DATA_ROOT := $(DEB_BUILD_ROOT)/data
DEB_CONTROL_ROOT := $(DEB_BUILD_ROOT)/control
DEB_VERIFY_ROOT := $(PACKAGE_OUT_DIR)/linqu_mem_service.deb.verify
DEB_PACKAGE := $(PACKAGE_OUT_DIR)/$(MEM_SERVICE_DEB_NAME)_$(MEM_SERVICE_DEB_VERSION)_$(MEM_SERVICE_DEB_ARCH).deb
DEB_LIST := $(DEB_PACKAGE).list
RPM_CC ?= aarch64-linux-gnu-gcc
RPM_HOST_CC ?= $(RPM_CC)
RPM_TOPDIR := $(PACKAGE_OUT_DIR)/rpmbuild
RPM_STAGE_ROOT := $(PACKAGE_OUT_DIR)/linqu_mem_service.rpm.stage
RPM_VERIFY_ROOT := $(PACKAGE_OUT_DIR)/linqu_mem_service.rpm.verify
RPM_SOURCE := $(RPM_TOPDIR)/SOURCES/$(MEM_SERVICE_RPM_NAME)-$(MEM_SERVICE_RPM_VERSION).tar.gz
RPM_PACKAGE := $(PACKAGE_OUT_DIR)/$(MEM_SERVICE_RPM_NAME)-$(MEM_SERVICE_RPM_VERSION)-$(MEM_SERVICE_RPM_RELEASE).$(MEM_SERVICE_RPM_ARCH).rpm
OPS_CERTIFICATION_EVIDENCE := $(PACKAGE_OUT_DIR)/ops-certification-linux-ci.evidence
OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER := $(PACKAGE_OUT_DIR)/ops-certification-upgrade-rollback.marker
OPS_CERTIFICATION_ROLLBACK_RPM ?=
OPS_CERTIFICATION_BUNDLE_ROOT := $(PACKAGE_OUT_DIR)/ops-certification-bundle
OPS_CERTIFICATION_BUNDLE := $(PACKAGE_OUT_DIR)/linqu-mem-service-ops-certification-bundle.tar
OPS_CERTIFICATION_BUNDLE_MANIFEST := $(OPS_CERTIFICATION_BUNDLE_ROOT)/ops-certification-bundle.manifest
OPS_CERTIFICATION_BUNDLE_VERIFY_ROOT := $(PACKAGE_OUT_DIR)/ops-certification-bundle.verify
REMOTE_TRANSPORT_EVIDENCE := $(PACKAGE_OUT_DIR)/remote-transport.evidence
REMOTE_TRANSPORT_BUNDLE_ROOT := $(PACKAGE_OUT_DIR)/remote-transport-bundle
REMOTE_TRANSPORT_BUNDLE := $(PACKAGE_OUT_DIR)/linqu-mem-service-remote-transport-bundle.tar
REMOTE_TRANSPORT_BUNDLE_MANIFEST := $(REMOTE_TRANSPORT_BUNDLE_ROOT)/remote-transport-bundle.manifest
REMOTE_TRANSPORT_BUNDLE_VERIFY_ROOT := $(PACKAGE_OUT_DIR)/remote-transport-bundle.verify
RELEASE_CERTIFICATION_VERIFY_ROOT := $(PACKAGE_OUT_DIR)/release-certification.verify
INSTALLED_SDK_EXAMPLE_SMOKE_DIR := $(PACKAGE_OUT_DIR)/installed-sdk-example-smoke
INSTALLED_SDK_PKGCONFIG_SMOKE_DIR := $(PACKAGE_OUT_DIR)/installed-sdk-pkgconfig-smoke
INSTALLED_SDK_RUNTIME_SMOKE_DIR := $(PACKAGE_OUT_DIR)/installed-sdk-runtime-smoke
INSTALL_BINDIR := $(DESTDIR)$(PREFIX)/bin
INSTALL_INCLUDEDIR := $(DESTDIR)$(PREFIX)/include/lingqu/mem_service
INSTALL_SRCDIR := $(DESTDIR)$(PREFIX)/src/lingqu/mem_service
INSTALL_DATADIR := $(DESTDIR)$(PREFIX)/share/lingqu/mem_service
INSTALL_PKGCONFIGDIR := $(DESTDIR)$(PREFIX)/lib/pkgconfig
INSTALL_EXAMPLEDIR := $(INSTALL_DATADIR)/examples
INSTALL_CONFIGDIR := $(INSTALL_DATADIR)/config
INSTALL_DEPLOYDIR := $(INSTALL_DATADIR)/deploy
INSTALL_SCRIPTSDIR := $(INSTALL_DATADIR)/scripts
INSTALL_HOSTDIR := $(DESTDIR)$(PREFIX)/libexec/lingqu/mem_service
INSTALL_SYSCONFDIR := $(DESTDIR)$(SYSCONFDIR)/lingqu/mem_service
INSTALL_SYSTEMDUNITDIR := $(DESTDIR)$(SYSTEMDUNITDIR)

MEM_SERVICE_PUBLIC_HEADERS := \
	$(ROOT)/components/mem_service/mem_service.h \
	$(ROOT)/components/mem_service/mem_service_core.h \
	$(ROOT)/components/mem_service/mem_service_client.h \
	$(ROOT)/components/mem_service/mem_service_wire.h \
	$(ROOT)/components/mem_service/mem_service_wire_client.h \
	$(ROOT)/components/mem_service/mem_service_wire_payload.h \
	$(ROOT)/components/mem_service/mem_service_wire_schema.h \
	$(ROOT)/components/mem_service/lingqu_object_service.h

MEM_SERVICE_CLIENT_SDK_SRCS := \
	$(ROOT)/components/mem_service/mem_service_client.c \
	$(ROOT)/components/mem_service/mem_service_wire_client.c

MEM_SERVICE_CLIENT_EXAMPLES := \
	examples/mem_service_serving_example.c \
	examples/mem_service_pretraining_example.c

MEM_SERVICE_CORE_SRCS := \
	mem_service.c \
	$(MEM_SERVICE_DAEMON) \
	$(MEM_SERVICE_CLIENT) \
	$(MEM_SERVICE_WIRE_CLIENT) \
	$(MEM_SERVICE_METADATA) \
	$(MEM_SERVICE_KEYS) \
	$(MEM_SERVICE_OBJECT_REFS) \
	$(MEM_SERVICE_UB_SSD_GSVA_BACKEND) \
	$(MEM_SERVICE_UB_SSD_GSVA_IO) \
	$(MEM_SERVICE_RECORDS)

MEM_SERVICE_QWEN3_ADAPTER_SRCS := \
	mem_service.c \
	$(MEM_SERVICE) \
	$(MEM_SERVICE_CLUSTER_UTILS) \
	$(MEM_SERVICE_CLUSTER_PAYLOAD) \
	$(MEM_SERVICE_CLUSTER_READ) \
	$(MEM_SERVICE_CLUSTER_RUNTIME) \
	$(MEM_SERVICE_CLUSTER_QUEUE) \
	$(MEM_SERVICE_CLUSTER_OBSERVE) \
	$(MEM_SERVICE_OBMM_OBJECT_FLOW) \
	$(MEM_SERVICE_DAEMON) \
	$(MEM_SERVICE_CLIENT) \
	$(MEM_SERVICE_WIRE_CLIENT) \
	$(MEM_SERVICE_METADATA) \
	$(MEM_SERVICE_KEYS) \
	$(MEM_SERVICE_OBJECT_REFS) \
	$(MEM_SERVICE_OBMM_OBJECTS) \
	$(MEM_SERVICE_UB_SSD_GSVA_BACKEND) \
	$(MEM_SERVICE_UB_SSD_GSVA_IO) \
	$(MEM_SERVICE_RECORDS) \
	$(MEM_SERVICE_QWEN3_RECORDS) \
	$(MEM_SERVICE_QWEN3_RUNTIME) \
	$(MEM_SERVICE_QWEN3_DECODE_BARRIER) \
	$(MEM_SERVICE_QWEN3_KV_STATE_FLOW) \
	$(MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW) \
	$(MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW) \
	$(MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW) \
	$(MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW) \
	$(MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW) \
	$(MEM_SERVICE_QWEN3) \
	$(LLM_INFER)

.PHONY: all host-artifact-smoke network-transport-block-smoke package-tarball package-tarball-smoke package-deb package-deb-smoke package-rpm package-rpm-smoke linux-ops-certification-smoke linux-ops-upgrade-rollback-smoke linux-ops-deployment-smoke remote-transport-evidence-verify remote-transport-certification-bundle remote-transport-certification-bundle-verify release-certification-verify install installed-sdk-example-smoke installed-sdk-pkgconfig-smoke installed-sdk-runtime-smoke install-smoke print-release-manifest print-package-manifest print-wire-schema print-admin-output-schema print-upgrade-rollback-policy print-ops-certification-policy print-alert-rules print-api-abi-policy print-compat-matrix print-compat-baseline-v1 print-compat-old-new-matrix clean

all: linqu_mem_service linqu_mem_service_core linqu_mem_service_qwen3

linqu_mem_service: $(MEM_SERVICE_CORE_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -lm -o $@

linqu_mem_service_core: $(MEM_SERVICE_CORE_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -lm -o $@

linqu_mem_service_qwen3: $(MEM_SERVICE_QWEN3_ADAPTER_SRCS)
	$(CC) $(CFLAGS) -DMEM_SERVICE_ENABLE_QWEN3_INSPECT $(INCLUDES) $^ -lm -o $@

linqu_mem_service_host: $(MEM_SERVICE_CORE_SRCS)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $^ -lm -o $@

host-artifact-smoke: linqu_mem_service_host
	./linqu_mem_service_host --smoke
	./linqu_mem_service_host version
	./linqu_mem_service_host version-fixtures
	./linqu_mem_service_host release-fixtures
	./linqu_mem_service_host package-fixtures
	./linqu_mem_service_host admin-output-fixtures
	./linqu_mem_service_host upgrade-rollback-fixtures
	./linqu_mem_service_host upgrade-rollback-runtime-fixtures
	./linqu_mem_service_host restore-policy-fixtures
	./linqu_mem_service_host runtime-quota-fixtures
	./linqu_mem_service_host retention-fixtures
	./linqu_mem_service_host checkpoint-retention-fixtures
	./linqu_mem_service_host payload-gc-fixtures
	./linqu_mem_service_host record-retention-fixtures
	./linqu_mem_service_host encryption-fixtures
	./linqu_mem_service_host alert-fixtures
	./linqu_mem_service_host alert-integration-fixtures
	./linqu_mem_service_host ops-certification-fixtures
	./linqu_mem_service_host ops-certification-evidence-fixtures
	./linqu_mem_service_host api-abi-fixtures
	./linqu_mem_service_host compat-runtime-fixtures
	./linqu_mem_service_host compat-old-server-runtime-fixtures
	./linqu_mem_service_host serving-fail-closed-fixtures
	./linqu_mem_service_host pretraining-fail-closed-fixtures
	./linqu_mem_service_host typed-payload-fixtures
	./linqu_mem_service_host transport-block-fixtures
	./linqu_mem_service_host ub-ssd-gsva-descriptor-fixtures
	./linqu_mem_service_host remote-block-backend-policy-fixtures
	./linqu_mem_service_host remote-transport-evidence-fixtures
	./linqu_mem_service_host collector-fixtures
	./linqu_mem_service_host deployment-fixtures

network-transport-block-smoke: linqu_mem_service_host
	./linqu_mem_service_host network-transport-block-fixtures

package-tarball:
	mkdir -p $(PACKAGE_OUT_DIR)
	rm -rf $(PACKAGE_STAGE_ROOT)
	$(MAKE) install DESTDIR=$(abspath $(PACKAGE_STAGE_ROOT)) PREFIX=$(PACKAGE_PREFIX) CC="$(CC)" CFLAGS="$(CFLAGS)" HOST_CC="$(HOST_CC)" HOST_CFLAGS="$(HOST_CFLAGS)"
	tar -cf $(PACKAGE_TARBALL) -C $(PACKAGE_STAGE_ROOT) usr etc

package-tarball-smoke: package-tarball
	test -f $(PACKAGE_TARBALL)
	tar -tf $(PACKAGE_TARBALL) > $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/bin/linqu_mem_service$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/libexec/lingqu/mem_service/linqu_mem_service_host$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/lib/pkgconfig/lingqu-mem-service.pc$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/package-manifest.txt$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/release-manifest.txt$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/ops-certification-policy.txt$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/config/mem_service.runtime.conf$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/config/mem_service.host.runtime.conf$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^etc/lingqu/mem_service/mem_service.conf$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^etc/lingqu/mem_service/mem_service.host.conf$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/lib/systemd/system/linqu_mem_service.service$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/lib/systemd/system/linqu_mem_service.host.service$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/examples/mem_service_serving_example.c$$' $(PACKAGE_TARBALL_LIST)
	grep -q '^usr/share/lingqu/mem_service/examples/mem_service_pretraining_example.c$$' $(PACKAGE_TARBALL_LIST)
	rm -rf $(PACKAGE_VERIFY_ROOT)
	mkdir -p $(PACKAGE_VERIFY_ROOT)
	tar -xf $(PACKAGE_TARBALL) -C $(PACKAGE_VERIFY_ROOT)
	test -x $(PACKAGE_VERIFY_ROOT)/usr/bin/linqu_mem_service
	test -x $(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host
	test -f $(PACKAGE_VERIFY_ROOT)/usr/lib/pkgconfig/lingqu-mem-service.pc
	test -f $(PACKAGE_VERIFY_ROOT)/usr/include/lingqu/mem_service/mem_service_client.h
	test -f $(PACKAGE_VERIFY_ROOT)/usr/src/lingqu/mem_service/mem_service_client.c
	test -f $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/ops-certification-policy.txt
	test -x $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh
	test -x $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh
	test -x $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh
	test -x $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh
	test -x $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh
	test -x $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh
	test -f $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/config/mem_service.runtime.conf
	test -f $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/config/mem_service.host.runtime.conf
	test -f $(PACKAGE_VERIFY_ROOT)/etc/lingqu/mem_service/mem_service.conf
	test -f $(PACKAGE_VERIFY_ROOT)/etc/lingqu/mem_service/mem_service.host.conf
	test -f $(PACKAGE_VERIFY_ROOT)/usr/lib/systemd/system/linqu_mem_service.service
	test -f $(PACKAGE_VERIFY_ROOT)/usr/lib/systemd/system/linqu_mem_service.host.service
	grep -q '^backend=snapshot+journal$$' $(PACKAGE_VERIFY_ROOT)/etc/lingqu/mem_service/mem_service.conf
	grep -q '^prefix=/usr$$' $(PACKAGE_VERIFY_ROOT)/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^Cflags: -I$${includedir}$$' $(PACKAGE_VERIFY_ROOT)/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^sdk_sources=$${sourcedir}/mem_service_client.c $${sourcedir}/mem_service_wire_client.c$$' $(PACKAGE_VERIFY_ROOT)/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^package_format=installed-layout-v1$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^artifact_format=tar$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^artifact_gate=package-tarball-smoke$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_certification_ci=scripts/run_mem_service_release_certification_ci.sh$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^pkgconfig=lib/pkgconfig/lingqu-mem-service.pc$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^file_class=release_scripts count=10$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^distributable_package_format=tar$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/release-manifest.txt
	grep -q '^distributable_package_gate=package-tarball-smoke$$' $(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/release-manifest.txt
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_package_sdk --dry-run | grep -q 'mem_service_serving_example unix:/tmp/linqu_mem_service_package_sdk/mem_service.sock'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_package_sdk --dry-run | grep -q 'mem_service_pretraining_example unix:/tmp/linqu_mem_service_package_sdk/mem_service.sock'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-payload-len 8192 --dry-run | grep -q 'ssh -o BatchMode=yes -o ConnectTimeout=10 producer-a $(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host --producer-payload-len 8192 --dry-run | grep -q 'ssh -o BatchMode=yes -o ConnectTimeout=10 producer-a'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-payload-len 8192 --dry-run | grep -q -- '--producer-ssh producer-a'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host --producer-payload-len 8192 --dry-run | grep -q 'producer-payload-len 8192'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --preflight --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(PACKAGE_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh --no-runtime
	$(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host --smoke
	$(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host package-fixtures
	$(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host release-fixtures
	$(PACKAGE_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host deployment-fixtures

package-deb:
	mkdir -p $(PACKAGE_OUT_DIR)
	rm -rf $(DEB_BUILD_ROOT)
	$(MAKE) install DESTDIR=$(abspath $(DEB_DATA_ROOT)) PREFIX=$(PACKAGE_PREFIX) CC="$(DEB_CC)" CFLAGS="$(CFLAGS)" HOST_CC="$(DEB_HOST_CC)" HOST_CFLAGS="$(HOST_CFLAGS)"
	mkdir -p $(DEB_CONTROL_ROOT)
	printf '%s\n' 'Package: $(MEM_SERVICE_DEB_NAME)' > $(DEB_CONTROL_ROOT)/control
	printf '%s\n' 'Version: $(MEM_SERVICE_DEB_VERSION)' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' 'Architecture: $(MEM_SERVICE_DEB_ARCH)' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' 'Maintainer: Lingqu simulator <noreply@example.invalid>' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' 'Section: devel' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' 'Priority: optional' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' 'Description: Lingqu mem_service independent daemon and SDK' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' ' Mem service daemon, client SDK, examples, release contracts, and deploy manifests.' >> $(DEB_CONTROL_ROOT)/control
	printf '%s\n' '2.0' > $(DEB_BUILD_ROOT)/debian-binary
	tar -C $(DEB_CONTROL_ROOT) -czf $(DEB_BUILD_ROOT)/control.tar.gz control
	tar -C $(DEB_DATA_ROOT) -czf $(DEB_BUILD_ROOT)/data.tar.gz usr etc
	rm -f $(DEB_PACKAGE)
	$(PYTHON) -c 'import pathlib,sys; out=pathlib.Path(sys.argv[1]); f=out.open("wb"); f.write(b"!<arch>\n"); [(f.write(b"%-16s%-12d%-6d%-6d%-8o%-10d`\n" % (name,0,0,0,0o100644,len(data))), f.write(data), f.write(b"\n" if len(data) % 2 else b"")) for raw in sys.argv[2:] for p in [pathlib.Path(raw)] for data in [p.read_bytes()] for name in [p.name.encode()]]; f.close()' $(abspath $(DEB_PACKAGE)) $(DEB_BUILD_ROOT)/debian-binary $(DEB_BUILD_ROOT)/control.tar.gz $(DEB_BUILD_ROOT)/data.tar.gz

package-deb-smoke: package-deb
	test -f $(DEB_PACKAGE)
	file $(DEB_PACKAGE) | grep -q 'Debian binary package'
	ar -t $(DEB_PACKAGE) > $(DEB_LIST)
	grep -q '^debian-binary$$' $(DEB_LIST)
	grep -q '^control.tar.gz$$' $(DEB_LIST)
	grep -q '^data.tar.gz$$' $(DEB_LIST)
	rm -rf $(DEB_VERIFY_ROOT)
	mkdir -p $(DEB_VERIFY_ROOT)
	cd $(DEB_VERIFY_ROOT) && ar -x $(abspath $(DEB_PACKAGE))
	grep -q '^2.0$$' $(DEB_VERIFY_ROOT)/debian-binary
	mkdir -p $(DEB_VERIFY_ROOT)/control $(DEB_VERIFY_ROOT)/data
	tar -xzf $(DEB_VERIFY_ROOT)/control.tar.gz -C $(DEB_VERIFY_ROOT)/control
	tar -xzf $(DEB_VERIFY_ROOT)/data.tar.gz -C $(DEB_VERIFY_ROOT)/data
	grep -q '^Package: $(MEM_SERVICE_DEB_NAME)$$' $(DEB_VERIFY_ROOT)/control/control
	grep -q '^Version: $(MEM_SERVICE_DEB_VERSION)$$' $(DEB_VERIFY_ROOT)/control/control
	grep -q '^Architecture: $(MEM_SERVICE_DEB_ARCH)$$' $(DEB_VERIFY_ROOT)/control/control
	test -x $(DEB_VERIFY_ROOT)/data/usr/bin/linqu_mem_service
	test -x $(DEB_VERIFY_ROOT)/data/usr/libexec/lingqu/mem_service/linqu_mem_service_host
	test -f $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	test -f $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/release-manifest.txt
	test -f $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/ops-certification-policy.txt
	test -f $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/config/mem_service.runtime.conf
	test -f $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/config/mem_service.host.runtime.conf
	test -x $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh
	test -x $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh
	test -x $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh
	test -x $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh
	test -x $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh
	test -x $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh
	test -f $(DEB_VERIFY_ROOT)/data/etc/lingqu/mem_service/mem_service.conf
	test -f $(DEB_VERIFY_ROOT)/data/etc/lingqu/mem_service/mem_service.host.conf
	test -f $(DEB_VERIFY_ROOT)/data/usr/lib/systemd/system/linqu_mem_service.service
	test -f $(DEB_VERIFY_ROOT)/data/usr/lib/systemd/system/linqu_mem_service.host.service
	grep -q '^backend=snapshot+journal$$' $(DEB_VERIFY_ROOT)/data/etc/lingqu/mem_service/mem_service.conf
	grep -q '^native_package_format=deb$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^native_package_gate=package-deb-smoke$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	test -f $(DEB_VERIFY_ROOT)/data/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^pkgconfig=lib/pkgconfig/lingqu-mem-service.pc$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^prefix=/usr$$' $(DEB_VERIFY_ROOT)/data/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^release_script_root=share/lingqu/mem_service/scripts$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_certification_ci=scripts/run_mem_service_release_certification_ci.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^file_class=release_scripts count=10$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^native_package_format=deb$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/release-manifest.txt
	grep -q '^native_package_gate=package-deb-smoke$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh$$' $(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/release-manifest.txt
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_deb_sdk --dry-run | grep -q 'mem_service_serving_example unix:/tmp/linqu_mem_service_deb_sdk/mem_service.sock'
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_deb_sdk --dry-run | grep -q 'mem_service_pretraining_example unix:/tmp/linqu_mem_service_deb_sdk/mem_service.sock'
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-payload-len 8192 --dry-run | grep -q 'ssh -o BatchMode=yes -o ConnectTimeout=10 producer-a $(DEB_VERIFY_ROOT)/data/usr/libexec/lingqu/mem_service/linqu_mem_service_host'
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-payload-len 8192 --dry-run | grep -q -- '--producer-ssh producer-a'
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --preflight --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(DEB_VERIFY_ROOT)/data/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh --no-runtime
	file $(DEB_VERIFY_ROOT)/data/usr/bin/linqu_mem_service | grep -q 'ELF 64-bit.*ARM aarch64'
	file $(DEB_VERIFY_ROOT)/data/usr/libexec/lingqu/mem_service/linqu_mem_service_host | grep -q 'ELF 64-bit.*ARM aarch64'

package-rpm:
	command -v rpmbuild >/dev/null
	command -v rpm2cpio >/dev/null
	command -v cpio >/dev/null
	mkdir -p $(PACKAGE_OUT_DIR)
	rm -rf $(RPM_TOPDIR) $(RPM_STAGE_ROOT)
	mkdir -p $(RPM_TOPDIR)/BUILD $(RPM_TOPDIR)/BUILDROOT $(RPM_TOPDIR)/RPMS $(RPM_TOPDIR)/SOURCES $(RPM_TOPDIR)/SPECS $(RPM_TOPDIR)/SRPMS
	$(MAKE) install DESTDIR=$(abspath $(RPM_STAGE_ROOT)) PREFIX=$(PACKAGE_PREFIX) CC="$(RPM_CC)" CFLAGS="$(CFLAGS)" HOST_CC="$(RPM_HOST_CC)" HOST_CFLAGS="$(HOST_CFLAGS)"
	tar -C $(RPM_STAGE_ROOT) -czf $(RPM_SOURCE) usr etc
	cp $(MEM_SERVICE_RPM_SPEC) $(RPM_TOPDIR)/SPECS/linqu-mem-service.spec
	rpmbuild -bb --target $(MEM_SERVICE_RPM_ARCH) --define "_topdir $(abspath $(RPM_TOPDIR))" --define "_mem_service_version $(MEM_SERVICE_RPM_VERSION)" --define "_mem_service_release $(MEM_SERVICE_RPM_RELEASE)" $(RPM_TOPDIR)/SPECS/linqu-mem-service.spec
	cp $(RPM_TOPDIR)/RPMS/$(MEM_SERVICE_RPM_ARCH)/$(MEM_SERVICE_RPM_NAME)-$(MEM_SERVICE_RPM_VERSION)-$(MEM_SERVICE_RPM_RELEASE).$(MEM_SERVICE_RPM_ARCH).rpm $(RPM_PACKAGE)

package-rpm-smoke: package-rpm
	test -f $(RPM_PACKAGE)
	rm -rf $(RPM_VERIFY_ROOT)
	mkdir -p $(RPM_VERIFY_ROOT)
	cd $(RPM_VERIFY_ROOT) && rpm2cpio $(abspath $(RPM_PACKAGE)) | cpio -idmu
	test -x $(RPM_VERIFY_ROOT)/usr/bin/linqu_mem_service
	test -x $(RPM_VERIFY_ROOT)/usr/libexec/lingqu/mem_service/linqu_mem_service_host
	test -f $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	test -f $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/release-manifest.txt
	test -f $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/ops-certification-policy.txt
	test -f $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/config/mem_service.runtime.conf
	test -f $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/config/mem_service.host.runtime.conf
	test -x $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh
	test -x $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh
	test -x $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh
	test -x $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh
	test -x $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh
	test -x $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh
	test -f $(RPM_VERIFY_ROOT)/etc/lingqu/mem_service/mem_service.conf
	test -f $(RPM_VERIFY_ROOT)/etc/lingqu/mem_service/mem_service.host.conf
	test -f $(RPM_VERIFY_ROOT)/usr/lib/systemd/system/linqu_mem_service.service
	test -f $(RPM_VERIFY_ROOT)/usr/lib/systemd/system/linqu_mem_service.host.service
	grep -q '^backend=snapshot+journal$$' $(RPM_VERIFY_ROOT)/etc/lingqu/mem_service/mem_service.conf
	grep -q '^rpm_native_package_format=rpm$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/release-manifest.txt
	grep -q '^rpm_native_package_gate=package-rpm-smoke$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/release-manifest.txt
	grep -q '^rpm_package_format=rpm$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^rpm_package_gate=package-rpm-smoke$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	test -f $(RPM_VERIFY_ROOT)/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^pkgconfig=lib/pkgconfig/lingqu-mem-service.pc$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^prefix=/usr$$' $(RPM_VERIFY_ROOT)/usr/lib/pkgconfig/lingqu-mem-service.pc
	grep -q '^release_script_root=share/lingqu/mem_service/scripts$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_certification_ci=scripts/run_mem_service_release_certification_ci.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^file_class=release_scripts count=10$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh$$' $(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/release-manifest.txt
	$(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_rpm_sdk --dry-run | grep -q 'mem_service_serving_example unix:/tmp/linqu_mem_service_rpm_sdk/mem_service.sock'
	$(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_rpm_sdk --dry-run | grep -q 'mem_service_pretraining_example unix:/tmp/linqu_mem_service_rpm_sdk/mem_service.sock'
	$(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(RPM_VERIFY_ROOT)/usr/share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --preflight --dry-run | grep -q 'release-readiness --ops-evidence-file'

linux-ops-certification-smoke: package-rpm-smoke linqu_mem_service_host
	test -f $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER)
	./linqu_mem_service_host ops-certification-linux-ci-smoke --rpm-file $(abspath $(RPM_PACKAGE)) --upgrade-rollback-marker $(abspath $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER)) --evidence-file $(abspath $(OPS_CERTIFICATION_EVIDENCE))

linux-ops-evidence-verify: linqu_mem_service_host
	test -f $(OPS_CERTIFICATION_EVIDENCE)
	./linqu_mem_service_host ops-certification-verify --evidence-file $(abspath $(OPS_CERTIFICATION_EVIDENCE))

linux-ops-certification-bundle: package-rpm-smoke linux-ops-evidence-verify
	test -f $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER)
	rm -rf $(OPS_CERTIFICATION_BUNDLE_ROOT)
	mkdir -p $(OPS_CERTIFICATION_BUNDLE_ROOT)
	cp $(OPS_CERTIFICATION_EVIDENCE) $(OPS_CERTIFICATION_BUNDLE_ROOT)/ops-certification-linux-ci.evidence
	cp $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER) $(OPS_CERTIFICATION_BUNDLE_ROOT)/ops-certification-upgrade-rollback.marker
	cp $(RPM_PACKAGE) $(OPS_CERTIFICATION_BUNDLE_ROOT)/$(notdir $(RPM_PACKAGE))
	cp $(MEM_SERVICE_RELEASE_MANIFEST) $(OPS_CERTIFICATION_BUNDLE_ROOT)/release-manifest.txt
	cp $(MEM_SERVICE_PACKAGE_MANIFEST) $(OPS_CERTIFICATION_BUNDLE_ROOT)/package-manifest.txt
	cp $(MEM_SERVICE_OPS_CERTIFICATION_POLICY) $(OPS_CERTIFICATION_BUNDLE_ROOT)/ops-certification-policy.txt
	printf '%s\n' 'bundle_schema=linqu-mem-service-ops-certification-bundle-v1' 'bundle_gate=linux-ops-certification-bundle' 'evidence_verify_gate=linux-ops-evidence-verify' 'evidence=ops-certification-linux-ci.evidence' 'upgrade_rollback_marker=ops-certification-upgrade-rollback.marker' 'rpm=$(notdir $(RPM_PACKAGE))' 'release_manifest=release-manifest.txt' 'package_manifest=package-manifest.txt' 'ops_certification_policy=ops-certification-policy.txt' > $(OPS_CERTIFICATION_BUNDLE_MANIFEST)
	tar -C $(OPS_CERTIFICATION_BUNDLE_ROOT) -cf $(OPS_CERTIFICATION_BUNDLE) .
	test -f $(OPS_CERTIFICATION_BUNDLE)

linux-ops-certification-bundle-verify: linqu_mem_service_host
	test -f $(OPS_CERTIFICATION_BUNDLE)
	$(ROOT)/scripts/verify_mem_service_ops_certification_bundle.sh --bundle-file $(abspath $(OPS_CERTIFICATION_BUNDLE)) --app-dir $(abspath .) --work-dir $(abspath $(OPS_CERTIFICATION_BUNDLE_VERIFY_ROOT))

remote-transport-evidence-verify: linqu_mem_service_host
	test -f $(REMOTE_TRANSPORT_EVIDENCE)
	./linqu_mem_service_host remote-transport-verify --evidence-file $(abspath $(REMOTE_TRANSPORT_EVIDENCE))

remote-transport-certification-bundle: remote-transport-evidence-verify
	rm -rf $(REMOTE_TRANSPORT_BUNDLE_ROOT)
	mkdir -p $(REMOTE_TRANSPORT_BUNDLE_ROOT)
	cp $(REMOTE_TRANSPORT_EVIDENCE) $(REMOTE_TRANSPORT_BUNDLE_ROOT)/remote-transport.evidence
	cp $(MEM_SERVICE_RELEASE_MANIFEST) $(REMOTE_TRANSPORT_BUNDLE_ROOT)/release-manifest.txt
	cp $(MEM_SERVICE_PACKAGE_MANIFEST) $(REMOTE_TRANSPORT_BUNDLE_ROOT)/package-manifest.txt
	printf '%s\n' 'bundle_schema=linqu-mem-service-remote-transport-bundle-v1' 'bundle_gate=remote-transport-certification-bundle' 'evidence_verify_gate=remote-transport-evidence-verify' 'evidence=remote-transport.evidence' 'release_manifest=release-manifest.txt' 'package_manifest=package-manifest.txt' > $(REMOTE_TRANSPORT_BUNDLE_MANIFEST)
	tar -C $(REMOTE_TRANSPORT_BUNDLE_ROOT) -cf $(REMOTE_TRANSPORT_BUNDLE) .
	test -f $(REMOTE_TRANSPORT_BUNDLE)

remote-transport-certification-bundle-verify: linqu_mem_service_host
	test -f $(REMOTE_TRANSPORT_BUNDLE)
	$(ROOT)/scripts/verify_mem_service_remote_transport_bundle.sh --bundle-file $(abspath $(REMOTE_TRANSPORT_BUNDLE)) --app-dir $(abspath .) --work-dir $(abspath $(REMOTE_TRANSPORT_BUNDLE_VERIFY_ROOT))

release-certification-verify: linqu_mem_service_host
	test -f $(OPS_CERTIFICATION_BUNDLE)
	test -f $(REMOTE_TRANSPORT_BUNDLE)
	$(ROOT)/scripts/verify_mem_service_release_certification.sh --ops-bundle-file $(abspath $(OPS_CERTIFICATION_BUNDLE)) --remote-transport-bundle-file $(abspath $(REMOTE_TRANSPORT_BUNDLE)) --app-dir $(abspath .) --work-dir $(abspath $(RELEASE_CERTIFICATION_VERIFY_ROOT))

linux-ops-upgrade-rollback-smoke: package-rpm-smoke
	test "$$(uname -s)" = Linux
	test "$$(id -u)" = 0
	test -d /run/systemd/system
	command -v rpm >/dev/null
	command -v systemctl >/dev/null
	command -v curl >/dev/null
	test -n "$(OPS_CERTIFICATION_ROLLBACK_RPM)"
	test -f "$(OPS_CERTIFICATION_ROLLBACK_RPM)"
	rpm -Uvh --replacepkgs $(abspath $(RPM_PACKAGE))
	systemctl daemon-reload
	systemctl restart linqu_mem_service.service
	systemctl restart linqu_mem_service.host.service
	systemctl is-active --quiet linqu_mem_service.service
	systemctl is-active --quiet linqu_mem_service.host.service
	curl -fsS http://127.0.0.1:9900/metrics | grep -q 'lingqu_mem_service_'
	curl -fsS http://127.0.0.1:9901/metrics | grep -q 'lingqu_mem_service_'
	rpm -Uvh --oldpackage --replacepkgs $(abspath $(OPS_CERTIFICATION_ROLLBACK_RPM))
	systemctl daemon-reload
	systemctl restart linqu_mem_service.service
	systemctl restart linqu_mem_service.host.service
	systemctl is-active --quiet linqu_mem_service.service
	systemctl is-active --quiet linqu_mem_service.host.service
	curl -fsS http://127.0.0.1:9900/metrics | grep -q 'lingqu_mem_service_'
	curl -fsS http://127.0.0.1:9901/metrics | grep -q 'lingqu_mem_service_'
	rpm -Uvh --replacepkgs $(abspath $(RPM_PACKAGE))
	systemctl daemon-reload
	systemctl restart linqu_mem_service.service
	systemctl restart linqu_mem_service.host.service
	systemctl is-active --quiet linqu_mem_service.service
	systemctl is-active --quiet linqu_mem_service.host.service
	curl -fsS http://127.0.0.1:9900/metrics | grep -q 'lingqu_mem_service_'
	curl -fsS http://127.0.0.1:9901/metrics | grep -q 'lingqu_mem_service_'
	mkdir -p $(PACKAGE_OUT_DIR)
	printf '%s\n' 'upgrade_rollback_deployment_smoke=pass' 'current_rpm=$(abspath $(RPM_PACKAGE))' 'rollback_rpm=$(abspath $(OPS_CERTIFICATION_ROLLBACK_RPM))' > $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER)

linux-ops-deployment-smoke: linux-ops-upgrade-rollback-smoke linqu_mem_service_host
	test "$$(uname -s)" = Linux
	test "$$(id -u)" = 0
	test -d /run/systemd/system
	command -v rpm >/dev/null
	command -v systemctl >/dev/null
	command -v journalctl >/dev/null
	command -v curl >/dev/null
	command -v promtool >/dev/null
	rpm -Uvh --replacepkgs $(abspath $(RPM_PACKAGE))
	systemctl daemon-reload
	systemctl restart linqu_mem_service.service
	systemctl restart linqu_mem_service.host.service
	systemctl is-active --quiet linqu_mem_service.service
	systemctl is-active --quiet linqu_mem_service.host.service
	curl -fsS http://127.0.0.1:9900/metrics | grep -q 'lingqu_mem_service_'
	curl -fsS http://127.0.0.1:9901/metrics | grep -q 'lingqu_mem_service_'
	promtool check rules /usr/share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml
	test -f $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER)
	./linqu_mem_service_host ops-certification-linux-ci-smoke --rpm-file $(abspath $(RPM_PACKAGE)) --upgrade-rollback-marker $(abspath $(OPS_CERTIFICATION_UPGRADE_ROLLBACK_MARKER)) --evidence-file $(abspath $(OPS_CERTIFICATION_EVIDENCE))

print-release-manifest:
	@cat $(MEM_SERVICE_RELEASE_MANIFEST)

print-package-manifest:
	@cat $(MEM_SERVICE_PACKAGE_MANIFEST)

print-wire-schema:
	@cat $(MEM_SERVICE_WIRE_SCHEMA_MANIFEST)

print-admin-output-schema:
	@cat $(MEM_SERVICE_ADMIN_OUTPUT_SCHEMA)

print-upgrade-rollback-policy:
	@cat $(MEM_SERVICE_UPGRADE_ROLLBACK_POLICY)

print-ops-certification-policy:
	@cat $(MEM_SERVICE_OPS_CERTIFICATION_POLICY)

print-alert-rules:
	@cat $(MEM_SERVICE_ALERT_RULES)

print-api-abi-policy:
	@cat $(MEM_SERVICE_API_ABI_POLICY)

print-compat-matrix:
	@cat $(MEM_SERVICE_COMPAT_MATRIX)

print-compat-baseline-v1:
	@cat $(MEM_SERVICE_COMPAT_BASELINE_V1)

print-compat-old-new-matrix:
	@cat $(MEM_SERVICE_COMPAT_OLD_NEW_MATRIX)

install: $(MEM_SERVICE_RELEASE_MANIFEST) $(MEM_SERVICE_PACKAGE_MANIFEST) $(MEM_SERVICE_WIRE_SCHEMA_MANIFEST) $(MEM_SERVICE_ADMIN_OUTPUT_SCHEMA) $(MEM_SERVICE_UPGRADE_ROLLBACK_POLICY) $(MEM_SERVICE_OPS_CERTIFICATION_POLICY) $(MEM_SERVICE_API_ABI_POLICY) $(MEM_SERVICE_COMPAT_MATRIX) $(MEM_SERVICE_COMPAT_BASELINE_V1) $(MEM_SERVICE_COMPAT_OLD_NEW_MATRIX) $(MEM_SERVICE_CONFIG_SCHEMA) $(MEM_SERVICE_CONFIG_EXAMPLE) $(MEM_SERVICE_CONFIG_RUNTIME) $(MEM_SERVICE_CONFIG_HOST_RUNTIME) $(MEM_SERVICE_DEPLOY_MANIFEST) $(MEM_SERVICE_HOST_DEPLOY_MANIFEST) $(MEM_SERVICE_ALERT_RULES) $(MEM_SERVICE_RELEASE_SCRIPTS)
	rm -f linqu_mem_service linqu_mem_service_host
	$(MAKE) -B linqu_mem_service CC="$(CC)" CFLAGS="$(CFLAGS)"
	$(MAKE) -B linqu_mem_service_host HOST_CC="$(HOST_CC)" HOST_CFLAGS="$(HOST_CFLAGS)"
	mkdir -p $(INSTALL_BINDIR) $(INSTALL_INCLUDEDIR) $(INSTALL_SRCDIR) $(INSTALL_DATADIR) $(INSTALL_PKGCONFIGDIR) $(INSTALL_EXAMPLEDIR) $(INSTALL_CONFIGDIR) $(INSTALL_DEPLOYDIR) $(INSTALL_SCRIPTSDIR) $(INSTALL_HOSTDIR) $(INSTALL_SYSCONFDIR) $(INSTALL_SYSTEMDUNITDIR)
	cp linqu_mem_service $(INSTALL_BINDIR)/linqu_mem_service
	cp linqu_mem_service_host $(INSTALL_HOSTDIR)/linqu_mem_service_host
	cp $(MEM_SERVICE_PUBLIC_HEADERS) $(INSTALL_INCLUDEDIR)/
	cp $(MEM_SERVICE_CLIENT_SDK_SRCS) $(INSTALL_SRCDIR)/
	cp $(MEM_SERVICE_CLIENT_EXAMPLES) $(INSTALL_EXAMPLEDIR)/
	cp $(MEM_SERVICE_RELEASE_MANIFEST) $(INSTALL_DATADIR)/release-manifest.txt
	cp $(MEM_SERVICE_PACKAGE_MANIFEST) $(INSTALL_DATADIR)/package-manifest.txt
	cp $(MEM_SERVICE_WIRE_SCHEMA_MANIFEST) $(INSTALL_DATADIR)/wire-schema.txt
	cp $(MEM_SERVICE_ADMIN_OUTPUT_SCHEMA) $(INSTALL_DATADIR)/admin-output-schema.txt
	cp $(MEM_SERVICE_UPGRADE_ROLLBACK_POLICY) $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	cp $(MEM_SERVICE_OPS_CERTIFICATION_POLICY) $(INSTALL_DATADIR)/ops-certification-policy.txt
	cp $(MEM_SERVICE_API_ABI_POLICY) $(INSTALL_DATADIR)/api-abi-policy.txt
	cp $(MEM_SERVICE_COMPAT_MATRIX) $(INSTALL_DATADIR)/compat-matrix.txt
	cp $(MEM_SERVICE_COMPAT_BASELINE_V1) $(INSTALL_DATADIR)/compat-baseline-v1.txt
	cp $(MEM_SERVICE_COMPAT_OLD_NEW_MATRIX) $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	cp $(MEM_SERVICE_CONFIG_SCHEMA) $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	cp $(MEM_SERVICE_CONFIG_EXAMPLE) $(INSTALL_CONFIGDIR)/mem_service.example.conf
	cp $(MEM_SERVICE_CONFIG_RUNTIME) $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	cp $(MEM_SERVICE_CONFIG_HOST_RUNTIME) $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	cp $(MEM_SERVICE_CONFIG_RUNTIME) $(INSTALL_SYSCONFDIR)/mem_service.conf
	cp $(MEM_SERVICE_CONFIG_HOST_RUNTIME) $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	cp $(MEM_SERVICE_DEPLOY_MANIFEST) $(INSTALL_DEPLOYDIR)/linqu_mem_service.service
	cp $(MEM_SERVICE_HOST_DEPLOY_MANIFEST) $(INSTALL_DEPLOYDIR)/linqu_mem_service.host.service
	cp $(MEM_SERVICE_DEPLOY_MANIFEST) $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.service
	cp $(MEM_SERVICE_HOST_DEPLOY_MANIFEST) $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.host.service
	cp $(MEM_SERVICE_ALERT_RULES) $(INSTALL_DEPLOYDIR)/linqu_mem_service.prometheus-alerts.yml
	cp $(MEM_SERVICE_RELEASE_SCRIPTS) $(INSTALL_SCRIPTSDIR)/
	printf '%s\n' 'prefix=$(PREFIX)' > $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'includedir=$${prefix}/include/lingqu/mem_service' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'sourcedir=$${prefix}/src/lingqu/mem_service' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' '' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'Name: Lingqu mem_service' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'Description: Lingqu mem_service source SDK for external serving and pretraining clients' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'Version: $(MEM_SERVICE_RPM_VERSION)' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'Cflags: -I$${includedir}' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'Libs:' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	printf '%s\n' 'sdk_sources=$${sourcedir}/mem_service_client.c $${sourcedir}/mem_service_wire_client.c' >> $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)

installed-sdk-example-smoke: install
	mkdir -p $(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -I$(INSTALL_INCLUDEDIR) $(INSTALL_EXAMPLEDIR)/mem_service_serving_example.c $(INSTALL_SRCDIR)/mem_service_client.c $(INSTALL_SRCDIR)/mem_service_wire_client.c -o $(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_serving_example
	$(HOST_CC) $(HOST_CFLAGS) -I$(INSTALL_INCLUDEDIR) $(INSTALL_EXAMPLEDIR)/mem_service_pretraining_example.c $(INSTALL_SRCDIR)/mem_service_client.c $(INSTALL_SRCDIR)/mem_service_wire_client.c -o $(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_pretraining_example
	test -x $(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_serving_example
	test -x $(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_pretraining_example

installed-sdk-pkgconfig-smoke: install
	command -v $(PKG_CONFIG) >/dev/null
	rm -rf $(INSTALLED_SDK_PKGCONFIG_SMOKE_DIR)
	mkdir -p $(INSTALLED_SDK_PKGCONFIG_SMOKE_DIR)
	set -eu; \
	export PKG_CONFIG_PATH="$(INSTALL_PKGCONFIGDIR)"; \
	$(PKG_CONFIG) --define-prefix --exists lingqu-mem-service; \
	cflags="$$($(PKG_CONFIG) --define-prefix --cflags lingqu-mem-service)"; \
	sdk_sources="$$($(PKG_CONFIG) --define-prefix --variable=sdk_sources lingqu-mem-service)"; \
	test -n "$$cflags"; \
	test -n "$$sdk_sources"; \
	$(HOST_CC) $(HOST_CFLAGS) $$cflags $(INSTALL_EXAMPLEDIR)/mem_service_serving_example.c $$sdk_sources -o $(INSTALLED_SDK_PKGCONFIG_SMOKE_DIR)/mem_service_serving_example; \
	$(HOST_CC) $(HOST_CFLAGS) $$cflags $(INSTALL_EXAMPLEDIR)/mem_service_pretraining_example.c $$sdk_sources -o $(INSTALLED_SDK_PKGCONFIG_SMOKE_DIR)/mem_service_pretraining_example
	test -x $(INSTALLED_SDK_PKGCONFIG_SMOKE_DIR)/mem_service_serving_example
	test -x $(INSTALLED_SDK_PKGCONFIG_SMOKE_DIR)/mem_service_pretraining_example

installed-sdk-runtime-smoke: installed-sdk-example-smoke
	rm -rf $(INSTALLED_SDK_RUNTIME_SMOKE_DIR)
	mkdir -p $(INSTALLED_SDK_RUNTIME_SMOKE_DIR)
	set -eu; \
	socket="$(abspath $(INSTALLED_SDK_RUNTIME_SMOKE_DIR))/mem_service.sock"; \
	store="$(abspath $(INSTALLED_SDK_RUNTIME_SMOKE_DIR))/store.snapshot"; \
	rm -f "$$socket" "$$store" "$$store.journal"; \
	$(INSTALL_HOSTDIR)/linqu_mem_service_host serve --listen unix:"$$socket" --store "$$store" > "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.stdout" 2> "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.stderr" & \
	pid=$$!; \
	trap 'kill "$$pid" >/dev/null 2>&1 || true; wait "$$pid" >/dev/null 2>&1 || true' EXIT INT TERM; \
	i=0; \
	while ! $(INSTALL_HOSTDIR)/linqu_mem_service_host ready --connect unix:"$$socket" >/dev/null 2>&1; do \
		if ! kill -0 "$$pid" >/dev/null 2>&1; then \
			cat "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.stderr"; \
			exit 1; \
		fi; \
		i=$$((i + 1)); \
		if [ "$$i" -ge 100 ]; then \
			cat "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.stderr"; \
			exit 1; \
		fi; \
		sleep 0.05; \
	done; \
	$(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_serving_example unix:"$$socket"; \
	$(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_pretraining_example unix:"$$socket"; \
	$(INSTALL_HOSTDIR)/linqu_mem_service_host status --connect unix:"$$socket"; \
	kill "$$pid" >/dev/null 2>&1 || true; \
	wait "$$pid" >/dev/null 2>&1 || true; \
	rm -f "$$socket"; \
	$(INSTALL_HOSTDIR)/linqu_mem_service_host serve --listen unix:"$$socket" --store "$$store" > "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.restart.stdout" 2> "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.restart.stderr" & \
	pid=$$!; \
	i=0; \
	while ! $(INSTALL_HOSTDIR)/linqu_mem_service_host ready --connect unix:"$$socket" >/dev/null 2>&1; do \
		if ! kill -0 "$$pid" >/dev/null 2>&1; then \
			cat "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.restart.stderr"; \
			exit 1; \
		fi; \
		i=$$((i + 1)); \
		if [ "$$i" -ge 100 ]; then \
			cat "$(INSTALLED_SDK_RUNTIME_SMOKE_DIR)/daemon.restart.stderr"; \
			exit 1; \
		fi; \
		sleep 0.05; \
	done; \
	$(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_serving_example unix:"$$socket"; \
	$(INSTALLED_SDK_EXAMPLE_SMOKE_DIR)/mem_service_pretraining_example unix:"$$socket"; \
	$(INSTALL_HOSTDIR)/linqu_mem_service_host status --connect unix:"$$socket"

install-smoke: install
	test -x $(INSTALL_BINDIR)/linqu_mem_service
	test -x $(INSTALL_HOSTDIR)/linqu_mem_service_host
	test -f $(INSTALL_INCLUDEDIR)/mem_service_client.h
	test -f $(INSTALL_INCLUDEDIR)/mem_service_wire.h
	test -f $(INSTALL_SRCDIR)/mem_service_client.c
	test -f $(INSTALL_SRCDIR)/mem_service_wire_client.c
	test -f $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	grep -q '^prefix=$(PREFIX)$$' $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	grep -q '^includedir=$${prefix}/include/lingqu/mem_service$$' $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	grep -q '^sourcedir=$${prefix}/src/lingqu/mem_service$$' $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	grep -q '^Cflags: -I$${includedir}$$' $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	grep -q '^sdk_sources=$${sourcedir}/mem_service_client.c $${sourcedir}/mem_service_wire_client.c$$' $(INSTALL_PKGCONFIGDIR)/$(MEM_SERVICE_PKGCONFIG_NAME)
	test -f $(INSTALL_EXAMPLEDIR)/mem_service_serving_example.c
	test -f $(INSTALL_EXAMPLEDIR)/mem_service_pretraining_example.c
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_layout.sh
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_sdk.sh
	test -x $(INSTALL_SCRIPTSDIR)/run_mem_service_linux_ops_ci.sh
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_linux_ops_evidence.sh
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_ops_certification_bundle.sh
	test -x $(INSTALL_SCRIPTSDIR)/run_mem_service_remote_transport_ci.sh
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_remote_transport_evidence.sh
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_remote_transport_bundle.sh
	test -x $(INSTALL_SCRIPTSDIR)/verify_mem_service_release_certification.sh
	test -x $(INSTALL_SCRIPTSDIR)/run_mem_service_release_certification_ci.sh
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_linux_ops_evidence.sh --evidence-file /tmp/linqu_mem_service_ops.evidence --dry-run | grep -q '$(INSTALL_HOSTDIR)/linqu_mem_service_host ops-certification-verify'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_remote_transport_evidence.sh --evidence-file /tmp/linqu_mem_service_remote_transport.evidence --dry-run | grep -q '$(INSTALL_HOSTDIR)/linqu_mem_service_host remote-transport-verify'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_ops_certification_bundle.sh --bundle-file /tmp/linqu_mem_service_ops_bundle.tar --dry-run | grep -q '$(INSTALL_HOSTDIR)/linqu_mem_service_host ops-certification-verify'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_remote_transport_bundle.sh --bundle-file /tmp/linqu_mem_service_remote_transport_bundle.tar --dry-run | grep -q '$(INSTALL_HOSTDIR)/linqu_mem_service_host remote-transport-verify'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_release_certification.sh --ops-bundle-file /tmp/linqu_mem_service_ops_bundle.tar --remote-transport-bundle-file /tmp/linqu_mem_service_remote_transport_bundle.tar --dry-run | grep -q 'verify_mem_service_ops_certification_bundle.sh --bundle-file /tmp/linqu_mem_service_ops_bundle.tar --work-dir'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_release_certification.sh --ops-bundle-file /tmp/linqu_mem_service_ops_bundle.tar --remote-transport-bundle-file /tmp/linqu_mem_service_remote_transport_bundle.tar --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_installed_sdk --dry-run | grep -q 'pkg-config --define-prefix --exists lingqu-mem-service'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_installed_sdk --dry-run | grep -q 'mem_service_serving_example unix:/tmp/linqu_mem_service_installed_sdk/mem_service.sock'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_installed_sdk --dry-run | grep -q 'mem_service_pretraining_example unix:/tmp/linqu_mem_service_installed_sdk/mem_service.sock'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_sdk.sh --work-dir /tmp/linqu_mem_service_installed_sdk --preflight --dry-run | grep -q 'pkg-config --define-prefix --variable=sdk_sources lingqu-mem-service'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_linux_ops_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --out-dir /tmp/linqu_mem_service_linux_ops --dry-run | grep -q 'ops-certification-linux-ci-smoke --rpm-file'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --out-dir /tmp/linqu_mem_service_remote_transport --dry-run | grep -q 'verify_mem_service_remote_transport_bundle.sh --bundle-file'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_remote_transport_ci.sh --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host --producer-payload-len 8192 --dry-run | grep -q 'ssh -o BatchMode=yes -o ConnectTimeout=10 producer-a'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --dry-run | grep -q 'verify_mem_service_release_certification.sh --ops-bundle-file'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --producer-ssh producer-a --producer-bin /usr/libexec/lingqu/mem_service/linqu_mem_service_host --producer-payload-len 8192 --dry-run | grep -q 'producer-payload-len 8192'
	$(INSTALL_SCRIPTSDIR)/run_mem_service_release_certification_ci.sh --rollback-rpm /tmp/linqu-mem-service-prev.rpm --source tcp:10.0.0.11:9000 --producer-host producer-a --consumer-host consumer-b --network-partition-marker /tmp/remote-transport.partition --preflight --dry-run | grep -q 'release-readiness --ops-evidence-file'
	$(INSTALL_SCRIPTSDIR)/verify_mem_service_installed_layout.sh --no-runtime
	test -f $(INSTALL_DATADIR)/release-manifest.txt
	test -f $(INSTALL_DATADIR)/package-manifest.txt
	test -f $(INSTALL_DATADIR)/wire-schema.txt
	test -f $(INSTALL_DATADIR)/admin-output-schema.txt
	test -f $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	test -f $(INSTALL_DATADIR)/ops-certification-policy.txt
	test -f $(INSTALL_DATADIR)/api-abi-policy.txt
	test -f $(INSTALL_DATADIR)/compat-matrix.txt
	test -f $(INSTALL_DATADIR)/compat-baseline-v1.txt
	test -f $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	test -f $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	test -f $(INSTALL_CONFIGDIR)/mem_service.example.conf
	test -f $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	test -f $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	test -f $(INSTALL_SYSCONFDIR)/mem_service.conf
	test -f $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	test -f $(INSTALL_DEPLOYDIR)/linqu_mem_service.service
	test -f $(INSTALL_DEPLOYDIR)/linqu_mem_service.host.service
	test -f $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.service
	test -f $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.host.service
	test -f $(INSTALL_DEPLOYDIR)/linqu_mem_service.prometheus-alerts.yml
	grep -q '^mem_service_release_manifest_version=1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^package_format=installed-layout-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^package_manifest=share/lingqu/mem_service/package-manifest.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^package_manifest_len=9369$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^package_manifest_checksum=0xa4d2a97f$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^package_gate=package-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_example_smoke=installed-sdk-example-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_example_smoke_scope=serving+pretraining-external-client-compile$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_runtime_smoke=installed-sdk-runtime-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_runtime_reuse=installed-sdk-runtime-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^distributable_package=out/mem_service/linqu_mem_service-installed-layout-v1.tar$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^distributable_package_format=tar$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^distributable_package_root=usr+etc$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^distributable_package_gate=package-tarball-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^native_package=out/mem_service/linqu-mem-service_0.1.0-1_arm64.deb$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^native_package_format=deb$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^native_package_arch=arm64$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^native_package_gate=package-deb-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^native_package_runtime=not-executed-cross-compiled-arm64$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rpm_native_package=out/mem_service/linqu-mem-service-0.1.0-1.aarch64.rpm$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rpm_native_package_format=rpm$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rpm_native_package_arch=aarch64$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rpm_native_package_gate=package-rpm-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rpm_native_package_runtime=requires-linux-rpm-toolchain$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^core_binary=bin/linqu_mem_service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^binary_version_command=version$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^binary_version_contract=text-kv$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^binary_version_gate=version-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_readiness_command=release-readiness$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_readiness_contract=text-kv$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_readiness_gate=release-readiness-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_daemon_artifact_smoke=host-artifact-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_schema_manifest=share/lingqu/mem_service/wire-schema.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_schema_manifest_checksum=0x14a081c9$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^admin_output_schema_len=6719$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^admin_output_schema_checksum=0x7a09d525$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^admin_output_format=text-kv$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^admin_metric_prefix=lingqu_mem_service_$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^upgrade_rollback_policy_len=2143$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^upgrade_rollback_policy_checksum=0x4315e596$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^upgrade_policy=current-version-only$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rollback_policy=current-version-only$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^old_server_runtime_binary=certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^upgrade_rollback_gate=upgrade-rollback-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_runtime_gate=compat-runtime-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_old_server_runtime_gate=compat-old-server-runtime-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^serving_fail_closed_matrix=certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^serving_fail_closed_gate=serving-fail-closed-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^pretraining_fail_closed_matrix=certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^pretraining_fail_closed_gate=pretraining-fail-closed-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^payload_ownership_matrix=certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^payload_ownership_scope=artifact-query-expected-owner$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^payload_ownership_gate=serving-fail-closed-fixtures,pretraining-fail-closed-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^restore_policy=transactional-staged-restore$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^restore_policy_scope=full-snapshot+paged-snapshot$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^restore_policy_gate=restore-policy-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^runtime_quota_admission=max-records+max-payload-bytes$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^runtime_quota_gate=runtime-quota-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^retention_policy=manual-or-audit-log-limit$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^retention_policy_gate=config-fixtures,retention-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^checkpoint_retention_policy=manual-or-latest-limit$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^record_retention_gate=config-fixtures,record-retention-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^payload_block_gc=record-and-checkpoint-retention-orphan-blocks$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^encryption_policy=explicit-none-only$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^encryption_at_rest=not-certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^encryption_policy_command=encryption-policy$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^encryption_policy_gate=encryption-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^restore_policy_fail_closed=bad-magic,future-store-schema,malformed-store-schema,out-of-order-page,record-count-mismatch,cancelled-stage-commit$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^restore_policy_live_state=unchanged-until-commit$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_payload_text_kv_format=text-kv$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_payload_typed_binary_format=typed-binary-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_payload_typed_binary_gate=typed-payload-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^api_abi_policy_len=856$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^api_abi_policy_checksum=0xd0cc1392$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_api_version=1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_abi_version=1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_record_abi_size=808$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_api_compatibility=source-compatible-within-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_abi_compatibility=wire-header-and-client-record-layout-stable-within-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^pkgconfig=lib/pkgconfig/lingqu-mem-service.pc$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^pkgconfig_name=lingqu-mem-service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^pkgconfig_cflags=-I$${includedir}$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^pkgconfig_sdk_sources=$${sourcedir}/mem_service_client.c $${sourcedir}/mem_service_wire_client.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_matrix=share/lingqu/mem_service/compat-matrix.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_matrix_len=1979$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_matrix_checksum=0xe6d3e50c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_baseline_len=1252$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_baseline_checksum=0xb93a31bc$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_old_new_matrix_len=1734$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^compat_old_new_matrix_checksum=0xbc0e044d$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^config_schema=share/lingqu/mem_service/config/mem_service.conf.schema$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^config_example=share/lingqu/mem_service/config/mem_service.example.conf$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^runtime_config=etc/lingqu/mem_service/mem_service.conf$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^service_auth_boundary=unix-socket-local-only$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_auth_boundary=loopback-only$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^config_security_gate=config-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^systemd_unit=lib/systemd/system/linqu_mem_service.service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^deployment_smoke=deployment-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_service_manager_smoke=installed-host-service-manager-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^collector_smoke=collector-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^collector_integration_smoke=installed-host-collector-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^collector_scrape_contract=prometheus-text-http-v0.0.4$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_rules_format=prometheus-rules-yaml$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_rules_len=2096$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_rules_checksum=0x05a9245c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_rule_count=6$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_rules_gate=alert-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^alert_integration_smoke=alert-integration-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_gate=ops-certification-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_evidence_schema=ops-certification-evidence-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_evidence_gate=ops-certification-evidence-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_generate=ops-certification-generate-evidence$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_certification_smoke=linux-ops-certification-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_evidence_verify=linux-ops-evidence-verify$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_certification_bundle=linux-ops-certification-bundle$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_certification_bundle_verify=linux-ops-certification-bundle-verify$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_certification_verify=release-certification-verify$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_certification_verify_script=scripts/verify_mem_service_release_certification.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script_root=share/lingqu/mem_service/scripts$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_linux_ops_evidence.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_evidence.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^linux_ops_deployment_smoke=linux-ops-deployment-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^ops_certification_verify=ops-certification-verify --evidence-file$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^real_systemd_environment=not-certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^production_collector_alert_environment=not-certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^rpm_package=not-certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^service_manager_lifecycle=serve-config-ready-scrape-sigterm$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^service_manager_shutdown=signal-clean-stop$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^durable_backend=snapshot+journal$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^durable_store_schema_version=1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^durable_store_migration_policy=legacy-to-v1-reject-future$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^durable_catalog=storage-root-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^durable_catalog_manifest=catalog/manifest.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1,ub-ssd-gsva-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_block_backend_gate=remote-block-backend-policy-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_block_data_gate=transport-block-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_network_transport=tcp-loopback-certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_network_transport_gate=network-transport-block-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_network_transport_make_gate=network-transport-block-smoke$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_network_transport=not-certified$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_generate=remote-transport-generate-evidence$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_verify=remote-transport-verify --evidence-file$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_evidence_verify=scripts/verify_mem_service_remote_transport_evidence.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_bundle=remote-transport-certification-bundle$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_bundle_verify=remote-transport-certification-bundle-verify$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^remote_payload_production_transport_bundle_script=scripts/verify_mem_service_remote_transport_bundle.sh$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^required_gate=remote-transport-evidence-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^payload_block_ingest=payload-inline,payload-file$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^durable_journal=store-path.journal$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_export_format=prometheus-text$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_listen_config=metrics_listen$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_http_listener=tcp-ipv4$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_scrape_path=/metrics$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_http_content_type=text/plain; version=0.0.4$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_retry_policy=explicit-max-attempts-backoff$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_api=pretraining-refs-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_api=pretraining-step-commit-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^operation=audit_log:10$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^public_header=include/lingqu/mem_service/mem_service_client.h$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_source=src/lingqu/mem_service/mem_service_client.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^example_source=share/lingqu/mem_service/examples/mem_service_serving_example.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^example_source=share/lingqu/mem_service/examples/mem_service_pretraining_example.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^mem_service_wire_schema_manifest_version=1$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^operation_count=23$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^field_count=164$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^oneof_field=resolve_kv_segment.0.block_hash$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^mem_service_admin_output_schema_version=1$$' $(INSTALL_DATADIR)/admin-output-schema.txt
	grep -q '^admin_command=metrics-export operation=metrics response=prometheus-text$$' $(INSTALL_DATADIR)/admin-output-schema.txt
	grep -q '^metrics_prometheus_prefix=lingqu_mem_service_$$' $(INSTALL_DATADIR)/admin-output-schema.txt
	grep -q '^metric_field=request_latency_max_ms type=gauge$$' $(INSTALL_DATADIR)/admin-output-schema.txt
	grep -q '^audit_record_delimiter=audit_begin/audit_end$$' $(INSTALL_DATADIR)/admin-output-schema.txt
	grep -q '^fail_closed_status=checksum_mismatch$$' $(INSTALL_DATADIR)/admin-output-schema.txt
	grep -q '^mem_service_upgrade_rollback_policy_version=1$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^upgrade_policy=current-version-only$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^rollback_policy=current-version-only$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^same_version_runtime_gate=upgrade-rollback-runtime-fixtures$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^old_server_runtime_binary=certified$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^new_client_old_server=certified$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^catalog_schema_version=1$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^migration_policy=catalog-schema-version-migrate-legacy-to-v1-reject-future$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^store_migration_policy=store-schema-version-migrate-legacy-to-v1-reject-future$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^required_gate=upgrade-rollback-runtime-fixtures$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^required_gate=compat-runtime-fixtures$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^required_gate=alert-fixtures$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^required_gate=alert-integration-fixtures$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^required_gate=package-fixtures$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^required_gate=install-smoke$$' $(INSTALL_DATADIR)/upgrade-rollback-policy.txt
	grep -q '^mem_service_package_manifest_version=1$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^package_format=installed-layout-v1$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^artifact_format=tar$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^artifact_name=linqu_mem_service-installed-layout-v1.tar$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^artifact_root=usr+etc$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^artifact_install_prefix=/usr$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^artifact_contents=installed-layout-v1-root$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^artifact_gate=package-tarball-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^native_package_format=deb$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^native_package_name=linqu-mem-service_0.1.0-1_arm64.deb$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^native_package_arch=arm64$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^native_package_payload=debian-binary+control.tar.gz+data.tar.gz$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^native_package_gate=package-deb-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^native_package_runtime=not-executed-cross-compiled-arm64$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^rpm_package_format=rpm$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^rpm_package_name=linqu-mem-service-0.1.0-1.aarch64.rpm$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^rpm_package_arch=aarch64$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^rpm_package_payload=rpm-cpio+metadata$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^rpm_package_gate=package-rpm-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^rpm_package_runtime=requires-linux-rpm-toolchain$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_file_count=46$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^pkgconfig=lib/pkgconfig/lingqu-mem-service.pc$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^pkgconfig_name=lingqu-mem-service$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^pkgconfig_cflags=-I$${includedir}$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^pkgconfig_sdk_sources=$${sourcedir}/mem_service_client.c $${sourcedir}/mem_service_wire_client.c$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^runtime_config=etc/lingqu/mem_service/mem_service.conf$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^service_auth_boundary=unix-socket-local-only$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^metrics_auth_boundary=loopback-only$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^config_security_gate=config-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^systemd_unit_root=lib/systemd/system$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^systemd_unit=lib/systemd/system/linqu_mem_service.service$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script_root=share/lingqu/mem_service/scripts$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_certification_ci=scripts/run_mem_service_release_certification_ci.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_sdk.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^binary_version_command=version$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^binary_version_contract=text-kv$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^binary_version_gate=version-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_readiness_command=release-readiness$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_readiness_contract=text-kv$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^release_readiness_gate=release-readiness-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^file_class=runtime_config count=2$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^file_class=systemd_units count=2$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^file_class=pkgconfig count=1$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^file_class=release_scripts count=10$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^runtime_quota_admission=max-records+max-payload-bytes$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^runtime_quota_gate=runtime-quota-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^retention_policy=manual-or-audit-log-limit$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^retention_policy_gate=config-fixtures,retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^checkpoint_retention_policy=manual-or-latest-limit$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^checkpoint_retention_gate=config-fixtures,checkpoint-retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^record_retention_policy=manual-or-global-kind-tenant-latest-or-ttl$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^record_retention_gate=config-fixtures,record-retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^payload_block_gc=record-and-checkpoint-retention-orphan-blocks$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^payload_block_gc_gate=payload-gc-fixtures,record-retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^encryption_policy=explicit-none-only$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^encryption_at_rest=not-certified$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^encryption_policy_command=encryption-policy$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^encryption_policy_gate=encryption-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate_count=34$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^contract=upgrade-rollback-policy path=share/lingqu/mem_service/upgrade-rollback-policy.txt checksum=0x4315e596$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^contract=ops-certification-policy path=share/lingqu/mem_service/ops-certification-policy.txt' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=package-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=version-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=release-readiness-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=upgrade-rollback-runtime-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=restore-policy-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=runtime-quota-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=checkpoint-retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=payload-gc-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=record-retention-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=encryption-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=compat-runtime-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=package-tarball-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=package-deb-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=package-rpm-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=installed-sdk-example-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=installed-sdk-pkgconfig-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=installed-sdk-runtime-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_runtime_smoke=installed-sdk-runtime-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_runtime_reuse=installed-sdk-runtime-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^installed_sdk_runtime_reuse_scope=daemon-restart+durable-store+serving+pretraining$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^restore_policy=transactional-staged-restore$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^restore_policy_gate=restore-policy-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=ops-certification-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=ops-certification-evidence-fixtures$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^required_gate=ops-certification-linux-ci-smoke$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^cross_version_upgrade=certified$$' $(INSTALL_DATADIR)/package-manifest.txt
	grep -q '^mem_service_ops_certification_policy_version=1$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^certification_status=not-certified$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^admission_rule=fail-closed-until-external-evidence$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^evidence_generate=ops-certification-generate-evidence$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^evidence_ci_gate=ops-certification-linux-ci-smoke$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^external_gate=linux-systemd-service-smoke$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^external_gate=prometheus-alertmanager-rule-smoke$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^external_gate=rpm-package-smoke$$' $(INSTALL_DATADIR)/ops-certification-policy.txt
	grep -q '^mem_service_api_abi_policy_version=1$$' $(INSTALL_DATADIR)/api-abi-policy.txt
	grep -q '^client_api_version=1$$' $(INSTALL_DATADIR)/api-abi-policy.txt
	grep -q '^client_abi_version=1$$' $(INSTALL_DATADIR)/api-abi-policy.txt
	grep -q '^client_record_abi_size=808$$' $(INSTALL_DATADIR)/api-abi-policy.txt
	grep -q '^old_client_new_server_policy=compatible-within-v1$$' $(INSTALL_DATADIR)/api-abi-policy.txt
	grep -q '^new_client_old_server_policy=certified$$' $(INSTALL_DATADIR)/api-abi-policy.txt
	grep -q '^mem_service_compat_matrix_version=1$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^wire_version_current=1$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^wire_schema_manifest_checksum=0x14a081c9$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^idempotency_conflict_status=version_conflict$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^idempotency_persistence=store-journal-and-full-snapshot$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^audit_log_persistence=store-journal-and-full-snapshot$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^journal_scope=completed-idempotency-and-audit-events$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^journal_truncation_policy=threshold-compaction$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^journal_truncation_policy=threshold-compaction$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^compat_test=deployment-fixtures$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^compat_test=compat-runtime-fixtures$$' $(INSTALL_DATADIR)/compat-matrix.txt
	grep -q '^mem_service_compat_baseline_version=1$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^old_client_new_server=compatible-within-v1$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^new_client_old_server=certified$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^compat_matrix_checksum=0xe6d3e50c$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^audit_log_persistence=store-journal-and-full-snapshot$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^baseline_payload=register_training_artifact:v1-training-step-compatible$$' $(INSTALL_DATADIR)/compat-baseline-v1.txt
	grep -q '^mem_service_old_new_compat_matrix_version=1$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^compat_baseline_checksum=0xb93a31bc$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^case=old-client-current-server:runtime-compatible$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^case=current-client-current-server:compat-runtime-fixtures$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^evidence=compat-runtime-fixtures$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^evidence=compat-old-server-runtime-fixtures$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^certified_pair=current-v1-client->old-v1-schema-profile$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^certified_pair=current-v1-client->old-v1-runtime-binary$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^certification_limit=none$$' $(INSTALL_DATADIR)/compat-old-new-matrix.txt
	grep -q '^mem_service_config_schema_version=1$$' $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	grep -q '^optional=record_retention$$' $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	grep -q '^field=record_retention type=enum values=manual,latest:<records>,ttl-ms:<age-ms>,kind:<record-kind>:latest:<records>,kind:<record-kind>:ttl-ms:<age-ms>,tenant:<owner-node>:latest:<records>,tenant:<owner-node>:ttl-ms:<age-ms> description=global,kind-scoped,or tenant-scoped record retention policy$$' $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	grep -q '^listen=unix:/tmp/linqu_mem_service.sock$$' $(INSTALL_CONFIGDIR)/mem_service.example.conf
	grep -q '^backend=snapshot+journal$$' $(INSTALL_CONFIGDIR)/mem_service.example.conf
	grep -q '^record_retention=manual$$' $(INSTALL_CONFIGDIR)/mem_service.example.conf
	grep -q '^metrics_listen=tcp:127.0.0.1:9900$$' $(INSTALL_CONFIGDIR)/mem_service.example.conf
	grep -q '^listen=unix:/run/lingqu/mem_service.sock$$' $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	grep -q '^store=/var/lib/lingqu/mem_service/store.snapshot$$' $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	grep -q '^storage_root=/var/lib/lingqu/mem_service$$' $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	grep -q '^backend=snapshot+journal$$' $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	grep -q '^record_retention=manual$$' $(INSTALL_CONFIGDIR)/mem_service.runtime.conf
	grep -q '^listen=unix:/run/lingqu/mem_service_host.sock$$' $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	grep -q '^store=/var/lib/lingqu/mem_service_host/store.snapshot$$' $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	grep -q '^storage_root=/var/lib/lingqu/mem_service_host$$' $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	grep -q '^record_retention=manual$$' $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	grep -q '^metrics_listen=tcp:127.0.0.1:9901$$' $(INSTALL_CONFIGDIR)/mem_service.host.runtime.conf
	grep -q '^listen=unix:/run/lingqu/mem_service.sock$$' $(INSTALL_SYSCONFDIR)/mem_service.conf
	grep -q '^store=/var/lib/lingqu/mem_service/store.snapshot$$' $(INSTALL_SYSCONFDIR)/mem_service.conf
	grep -q '^storage_root=/var/lib/lingqu/mem_service$$' $(INSTALL_SYSCONFDIR)/mem_service.conf
	grep -q '^backend=snapshot+journal$$' $(INSTALL_SYSCONFDIR)/mem_service.conf
	grep -q '^record_retention=manual$$' $(INSTALL_SYSCONFDIR)/mem_service.conf
	grep -q '^metrics_listen=tcp:127.0.0.1:9900$$' $(INSTALL_SYSCONFDIR)/mem_service.conf
	grep -q '^listen=unix:/run/lingqu/mem_service_host.sock$$' $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	grep -q '^store=/var/lib/lingqu/mem_service_host/store.snapshot$$' $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	grep -q '^storage_root=/var/lib/lingqu/mem_service_host$$' $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	grep -q '^record_retention=manual$$' $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	grep -q '^metrics_listen=tcp:127.0.0.1:9901$$' $(INSTALL_SYSCONFDIR)/mem_service.host.conf
	grep -q '^ExecStart=/usr/bin/linqu_mem_service serve --config /etc/lingqu/mem_service/mem_service.conf$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.service
	grep -q '^ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host serve --config /etc/lingqu/mem_service/mem_service.host.conf$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.host.service
	grep -q '^ExecStart=/usr/bin/linqu_mem_service serve --config /etc/lingqu/mem_service/mem_service.conf$$' $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.service
	grep -q '^ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host serve --config /etc/lingqu/mem_service/mem_service.host.conf$$' $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.host.service
	grep -q '^RuntimeDirectory=lingqu$$' $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.service
	grep -q '^StateDirectory=lingqu/mem_service$$' $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.service
	grep -q '^RuntimeDirectory=lingqu$$' $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.host.service
	grep -q '^StateDirectory=lingqu/mem_service_host$$' $(INSTALL_SYSTEMDUNITDIR)/linqu_mem_service.host.service
	grep -q '^  - alert: LingquMemServiceDown$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.prometheus-alerts.yml
	grep -q '^    expr: increase(lingqu_mem_service_fail_closed_count\[5m\]) > 0$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.prometheus-alerts.yml
	grep -q '^    expr: increase(lingqu_mem_service_capacity_exceeded_count\[5m\]) > 0$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.prometheus-alerts.yml
	grep -q '^    expr: lingqu_mem_service_request_latency_max_ms > 100$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.prometheus-alerts.yml
	$(INSTALL_HOSTDIR)/linqu_mem_service_host version
	$(INSTALL_HOSTDIR)/linqu_mem_service_host version-fixtures
	$(INSTALL_HOSTDIR)/linqu_mem_service_host release-readiness | grep -q '^overall_status=not-certified$$'
	$(INSTALL_HOSTDIR)/linqu_mem_service_host release-readiness | grep -q '^release_certification_verify=scripts/verify_mem_service_release_certification.sh --ops-bundle-file --remote-transport-bundle-file$$'
	$(INSTALL_HOSTDIR)/linqu_mem_service_host release-readiness | grep -q '^release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file$$'
	$(INSTALL_HOSTDIR)/linqu_mem_service_host release-readiness-fixtures

clean:
	rm -f linqu_mem_service linqu_mem_service_core linqu_mem_service_qwen3 linqu_mem_service_host

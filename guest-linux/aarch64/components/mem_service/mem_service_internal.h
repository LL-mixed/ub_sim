#ifndef MEM_SERVICE_INTERNAL_H
#define MEM_SERVICE_INTERNAL_H

#include "mem_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "mem_service_cluster_payload_contract.h"
#include "mem_service_compiler.h"
#include "mem_service_guest_runtime.h"
#include "mem_service_object_contract.h"
#include "mem_service_profile.h"
#include "mem_service_runtime_config.h"

#ifndef major
#define major(dev) ((unsigned int)(((uint64_t)(dev) >> 24) & 0xffU))
#endif
#ifndef minor
#define minor(dev) ((unsigned int)((uint64_t)(dev) & 0xffffffU))
#endif

#endif

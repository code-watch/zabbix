/*
** Zabbix
** Copyright (C) 2001-2016 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "ipc.h"
#include "memalloc.h"
#include "log.h"
#include "zbxalgo.h"
#include "daemon.h"
#include "zbxself.h"

#include "vmware.h"

/*
 * The VMware data (zbx_vmware_service_t structure) are stored in shared memory.
 * This data can be accessed with zbx_vmware_get_service() function and is regularly
 * updated by VMware collector processes.
 *
 * When a new service is requested by poller the zbx_vmware_get_service() function
 * creates a new service object, marks it as new, but still returns NULL object.
 *
 * The collectors check the service object list for new services or services not updated
 * during last CONFIG_VMWARE_FREQUENCY seconds. If such service is found it is marked
 * as updating.
 *
 * The service object is updated by creating a new data object, initializing it
 * with the latest data from VMware vCenter (or Hypervisor), destroying the old data
 * object and replacing it with the new one.
 *
 * The collector must be locked only when accessing service object list and working with
 * a service object. It is not locked for new data object creation during service update,
 * which is the most time consuming task.
 *
 * As the data retrieved by VMware collector can be quite big (for example 1 Hypervisor
 * with 500 Virtual Machines will result in approximately 20 MB of data), VMware collector
 * updates performance data (which is only 10% of the structure data) separately
 * with CONFIG_VMWARE_PERF_FREQUENCY period. The performance data is stored directly
 * in VMware service object entities vector - so the structure data is not affected by
 * performance data updates.
 */

extern char		*CONFIG_FILE;
extern int		CONFIG_VMWARE_FREQUENCY;
extern int		CONFIG_VMWARE_PERF_FREQUENCY;
extern zbx_uint64_t	CONFIG_VMWARE_CACHE_SIZE;
extern int		CONFIG_VMWARE_TIMEOUT;

extern unsigned char	process_type, program_type;
extern int		server_num, process_num;
extern char		*CONFIG_SOURCE_IP;

#define VMWARE_VECTOR_CREATE(ref, type)	zbx_vector_##type##_create_ext(ref,  __vm_mem_malloc_func, \
		__vm_mem_realloc_func, __vm_mem_free_func)

#define ZBX_VMWARE_CACHE_UPDATE_PERIOD	CONFIG_VMWARE_FREQUENCY
#define ZBX_VMWARE_PERF_UPDATE_PERIOD	CONFIG_VMWARE_PERF_FREQUENCY
#define ZBX_VMWARE_SERVICE_TTL	SEC_PER_DAY

static ZBX_MUTEX	vmware_lock = ZBX_MUTEX_NULL;

static zbx_mem_info_t	*vmware_mem = NULL;

ZBX_MEM_FUNC_IMPL(__vm, vmware_mem)

static zbx_vmware_t	*vmware = NULL;

/* vmware service types */
#define ZBX_VMWARE_TYPE_UNKNOWN	0
#define ZBX_VMWARE_TYPE_VSPHERE	1
#define ZBX_VMWARE_TYPE_VCENTER	2

#if defined(HAVE_LIBXML2) && defined(HAVE_LIBCURL)

#define ZBX_VMWARE_COUNTERS_INIT_SIZE	500

/* VMware service object name mapping for vcenter and vsphere installations */
typedef struct
{
	const char	*performance_manager;
	const char	*session_manager;
	const char	*event_manager;
	const char	*property_collector;
}
zbx_vmware_service_objects_t;

static zbx_vmware_service_objects_t	vmware_service_objects[3] =
{
	{NULL, NULL, NULL, NULL},
	{"ha-perfmgr", "ha-sessionmgr", "ha-eventmgr", "ha-property-collector"},
	{"PerfMgr", "SessionManager", "EventManager", "propertyCollector"}
};

/* mapping of performance counter group/key[rollup type] to its id (net/transmitted[average] -> <id>) */
typedef struct
{
	char		*path;
	zbx_uint64_t	id;
}
zbx_vmware_counter_t;

/*
 * SOAP support
 */
#define	ZBX_XML_HEADER1		"Soapaction:urn:vim25/4.1"
#define ZBX_XML_HEADER2		"Content-Type:text/xml; charset=utf-8"

#define ZBX_POST_VSPHERE_HEADER									\
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"					\
		"<SOAP-ENV:Envelope"								\
			" xmlns:ns0=\"urn:vim25\""						\
			" xmlns:ns1=\"http://schemas.xmlsoap.org/soap/envelope/\""		\
			" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""		\
			" xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\">"	\
			"<SOAP-ENV:Header/>"							\
			"<ns1:Body>"
#define ZBX_POST_VSPHERE_FOOTER									\
			"</ns1:Body>"								\
		"</SOAP-ENV:Envelope>"

#define ZBX_XPATH_FAULTSTRING()										\
	"/*/*/*[local-name()='Fault']/*[local-name()='faultstring']"
#define ZBX_XPATH_REFRESHRATE()										\
	"/*/*/*/*/*[local-name()='refreshRate']"
#define ZBX_XPATH_COUNTERINFO()										\
	"/*/*/*/*/*[local-name()='propSet']/*[local-name()='val']/*[local-name()='PerfCounterInfo']"
#define ZBX_XPATH_DATASTORE(property)									\
	"/*/*/*/*/*/*[local-name()='propSet']/*[local-name()='val']"					\
	"/*[local-name()='" property "']"
#define ZBX_XPATH_HV_DATASTORES()									\
	"/*/*/*/*/*[local-name()='propSet'][*[local-name()='name'][text()='datastore']]"		\
	"/*[local-name()='val']/*[@type='Datastore']"
#define ZBX_XPATH_HV_VMS()										\
	"/*/*/*/*/*[local-name()='propSet'][*[local-name()='name'][text()='vm']]"			\
	"/*[local-name()='val']/*[@type='VirtualMachine']"

typedef struct
{
	char	*data;
	size_t	alloc;
	size_t	offset;
}
ZBX_HTTPPAGE;

static ZBX_HTTPPAGE	page;

static size_t	curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t	r_size = size * nmemb;

	zbx_strncpy_alloc(&page.data, &page.alloc, &page.offset, ptr, r_size);

	return r_size;
}

static size_t	curl_header_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	return size * nmemb;
}

/******************************************************************************
 *                                                                            *
 * performance counter hashset support functions                              *
 *                                                                            *
 ******************************************************************************/
static zbx_hash_t	vmware_counter_hash_func(const void *data)
{
	zbx_vmware_counter_t	*counter = (zbx_vmware_counter_t *)data;

	return ZBX_DEFAULT_STRING_HASH_ALGO(counter->path, strlen(counter->path), ZBX_DEFAULT_HASH_SEED);
}

static int	vmware_counter_compare_func(const void *d1, const void *d2)
{
	zbx_vmware_counter_t	*c1 = (zbx_vmware_counter_t *)d1;
	zbx_vmware_counter_t	*c2 = (zbx_vmware_counter_t *)d2;

	return strcmp(c1->path, c2->path);
}

/******************************************************************************
 *                                                                            *
 * performance entities hashset support functions                             *
 *                                                                            *
 ******************************************************************************/
static zbx_hash_t	vmware_perf_entity_hash_func(const void *data)
{
	zbx_hash_t	seed;

	zbx_vmware_perf_entity_t	*entity = (zbx_vmware_perf_entity_t *)data;

	seed = ZBX_DEFAULT_STRING_HASH_ALGO(entity->type, strlen(entity->type), ZBX_DEFAULT_HASH_SEED);

	return ZBX_DEFAULT_STRING_HASH_ALGO(entity->id, strlen(entity->id), seed);
}

static int	vmware_perf_entity_compare_func(const void *d1, const void *d2)
{
	int	ret;

	zbx_vmware_perf_entity_t	*e1 = (zbx_vmware_perf_entity_t *)d1;
	zbx_vmware_perf_entity_t	*e2 = (zbx_vmware_perf_entity_t *)d2;

	if (0 == (ret = strcmp(e1->type, e2->type)))
		ret = strcmp(e1->id, e2->id);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_shared_strdup                                             *
 *                                                                            *
 * Purpose: duplicates the specified string into shared memory                *
 *                                                                            *
 * Parameters: source - [IN] the source string                                *
 *                                                                            *
 * Return value: a pointer to the duplicated string                           *
 *                                                                            *
 * Comments: The program execution is aborted if there is insufficient free   *
 *           shared memory to duplicate the string                            *
 *                                                                            *
 ******************************************************************************/
static char	*vmware_shared_strdup(const char *source)
{
	char	*ptr = NULL;
	size_t	len;

	if (NULL != source)
	{
		len = strlen(source) + 1;
		ptr = __vm_mem_malloc_func(NULL, len);
		memcpy(ptr, source, len);
	}

	return ptr;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_counters_shared_copy                                      *
 *                                                                            *
 * Purpose: copies performance counter vector into shared memory hashset      *
 *                                                                            *
 * Parameters: dst - [IN] the destination hashset                             *
 *             src - [IN] the source vector                                   *
 *                                                                            *
 ******************************************************************************/
static void	vmware_counters_shared_copy(zbx_hashset_t *dst, const zbx_vector_ptr_t *src)
{
	int			i;
	zbx_vmware_counter_t	*csrc, *cdst;

	for (i = 0; i < src->values_num; i++)
	{
		csrc = src->values[i];

		cdst = zbx_hashset_insert(dst, csrc, sizeof(zbx_vmware_counter_t));
		cdst->path = vmware_shared_strdup(csrc->path);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_vector_ptr_pair_shared_clean                              *
 *                                                                            *
 * Purpose: frees shared resources allocated to store instance performance    *
 *          counter values                                                    *
 *                                                                            *
 * Parameters: instance - [IN] the performance counter instance value         *
 *                                                                            *
 ******************************************************************************/
static void	vmware_vector_ptr_pair_shared_clean(zbx_vector_ptr_pair_t *pairs)
{
	int	i;

	for (i = 0; i < pairs->values_num; i++)
	{
		zbx_ptr_pair_t	*pair = &pairs->values[i];

		if (NULL != pair->first)
			__vm_mem_free_func(pair->first);

		__vm_mem_free_func(pair->second);
	}

	pairs->values_num = 0;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_perf_counter_shared_free                                  *
 *                                                                            *
 * Purpose: frees shared resources allocated to store performance counter     *
 *          data                                                              *
 *                                                                            *
 * Parameters: counter - [IN] the performance counter data                    *
 *                                                                            *
 ******************************************************************************/
static void	vmware_perf_counter_shared_free(zbx_vmware_perf_counter_t *counter)
{
	vmware_vector_ptr_pair_shared_clean(&counter->values);
	zbx_vector_ptr_pair_destroy(&counter->values);
	__vm_mem_free_func(counter);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_entities_shared_clean_stats                               *
 *                                                                            *
 * Purpose: removes statistics data from vmware entities                      *
 *                                                                            *
 ******************************************************************************/
static void	vmware_entities_shared_clean_stats(zbx_hashset_t *entities)
{
	int				i;
	zbx_vmware_perf_entity_t	*entity;
	zbx_vmware_perf_counter_t	*counter;
	zbx_hashset_iter_t		iter;


	zbx_hashset_iter_reset(entities, &iter);
	while (NULL != (entity = zbx_hashset_iter_next(&iter)))
	{
		for (i = 0; i < entity->counters.values_num; i++)
		{
			counter = (zbx_vmware_perf_counter_t *)entity->counters.values[i];
			vmware_vector_ptr_pair_shared_clean(&counter->values);

			if (0 != (counter->state & ZBX_VMWARE_COUNTER_UPDATING))
				counter->state = ZBX_VMWARE_COUNTER_READY;
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_datastore_shared_free                                     *
 *                                                                            *
 * Purpose: frees shared resources allocated to store datastore data          *
 *                                                                            *
 * Parameters: datastore   - [IN] the datastore                               *
 *                                                                            *
 ******************************************************************************/
static void	vmware_datastore_shared_free(zbx_vmware_datastore_t *datastore)
{
	__vm_mem_free_func(datastore->name);

	if (NULL != datastore->uuid)
		__vm_mem_free_func(datastore->uuid);

	__vm_mem_free_func(datastore);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_dev_shared_free                                           *
 *                                                                            *
 * Purpose: frees shared resources allocated to store vm device data          *
 *                                                                            *
 * Parameters: dev   - [IN] the vm device                                     *
 *                                                                            *
 ******************************************************************************/
static void	vmware_dev_shared_free(zbx_vmware_dev_t *dev)
{
	if (NULL != dev->instance)
		__vm_mem_free_func(dev->instance);

	if (NULL != dev->label)
		__vm_mem_free_func(dev->label);

	__vm_mem_free_func(dev);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_vm_shared_free                                            *
 *                                                                            *
 * Purpose: frees shared resources allocated to store virtual machine         *
 *                                                                            *
 * Parameters: vm   - [IN] the virtual machine                                *
 *                                                                            *
 ******************************************************************************/
static void	vmware_vm_shared_free(zbx_vmware_vm_t *vm)
{
	zbx_vector_ptr_clear_ext(&vm->devs, (zbx_clean_func_t)vmware_dev_shared_free);
	zbx_vector_ptr_destroy(&vm->devs);

	if (NULL != vm->uuid)
		__vm_mem_free_func(vm->uuid);

	if (NULL != vm->id)
		__vm_mem_free_func(vm->id);

	if (NULL != vm->details)
		__vm_mem_free_func(vm->details);

	__vm_mem_free_func(vm);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_hv_shared_free                                            *
 *                                                                            *
 * Purpose: frees shared resources allocated to store vmware hypervisor       *
 *                                                                            *
 * Parameters: hv   - [IN] the vmware hypervisor                              *
 *                                                                            *
 ******************************************************************************/
static void	vmware_hv_shared_free(zbx_vmware_hv_t *hv)
{
	zbx_vector_ptr_clear_ext(&hv->datastores, (zbx_clean_func_t)vmware_datastore_shared_free);
	zbx_vector_ptr_destroy(&hv->datastores);

	zbx_vector_ptr_clear_ext(&hv->vms, (zbx_clean_func_t)vmware_vm_shared_free);
	zbx_vector_ptr_destroy(&hv->vms);

	if (NULL != hv->uuid)
		__vm_mem_free_func(hv->uuid);

	if (NULL != hv->id)
		__vm_mem_free_func(hv->id);

	if (NULL != hv->details)
		__vm_mem_free_func(hv->details);

	if (NULL != hv->clusterid)
		__vm_mem_free_func(hv->clusterid);

	__vm_mem_free_func(hv);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_cluster_shared_free                                       *
 *                                                                            *
 * Purpose: frees shared resources allocated to store vmware cluster          *
 *                                                                            *
 * Parameters: cluster   - [IN] the vmware cluster                            *
 *                                                                            *
 ******************************************************************************/
static void	vmware_cluster_shared_free(zbx_vmware_cluster_t *cluster)
{
	if (NULL != cluster->name)
		__vm_mem_free_func(cluster->name);

	if (NULL != cluster->id)
		__vm_mem_free_func(cluster->id);

	if (NULL != cluster->status)
		__vm_mem_free_func(cluster->status);

	__vm_mem_free_func(cluster);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_data_shared_free                                          *
 *                                                                            *
 * Purpose: frees shared resources allocated to store vmware service data     *
 *                                                                            *
 * Parameters: data   - [IN] the vmware service data                          *
 *                                                                            *
 ******************************************************************************/
static void	vmware_data_shared_free(zbx_vmware_data_t *data)
{
	if (NULL != data)
	{
		zbx_vector_ptr_clear_ext(&data->hvs, (zbx_clean_func_t)vmware_hv_shared_free);
		zbx_vector_ptr_destroy(&data->hvs);

		zbx_vector_ptr_clear_ext(&data->clusters, (zbx_clean_func_t)vmware_cluster_shared_free);
		zbx_vector_ptr_destroy(&data->clusters);

		if (NULL != data->events)
			__vm_mem_free_func(data->events);

		if (NULL != data->error)
			__vm_mem_free_func(data->error);

		__vm_mem_free_func(data);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_shared_perf_entity_clean                                  *
 *                                                                            *
 * Purpose: cleans resources allocated by vmware peformance entity in vmware  *
 *          cache                                                             *
 *                                                                            *
 * Parameters: entity - [IN] the entity to free                               *
 *                                                                            *
 ******************************************************************************/
static void	vmware_shared_perf_entity_clean(zbx_vmware_perf_entity_t *entity)
{
	zbx_vector_ptr_clear_ext(&entity->counters, (zbx_mem_free_func_t)vmware_perf_counter_shared_free);
	zbx_vector_ptr_destroy(&entity->counters);

	__vm_mem_free_func(entity->type);
	__vm_mem_free_func(entity->id);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_counter_shared_clean                                      *
 *                                                                            *
 * Purpose: frees resources allocated by vmware performance counter           *
 *                                                                            *
 * Parameters: counter - [IN] the performance counter to free                 *
 *                                                                            *
 ******************************************************************************/
static void	vmware_counter_shared_clean(zbx_vmware_counter_t *counter)
{
	__vm_mem_free_func(counter->path);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_shared_free                                       *
 *                                                                            *
 * Purpose: frees shared resources allocated to store vmware service          *
 *                                                                            *
 * Parameters: data   - [IN] the vmware service data                          *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_shared_free(zbx_vmware_service_t *service)
{
	const char			*__function_name = "vmware_service_shared_free";

	zbx_hashset_iter_t		iter;
	zbx_vmware_counter_t		*counter;
	zbx_vmware_perf_entity_t	*entity;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() '%s'@'%s'", __function_name, service->username, service->url);

	__vm_mem_free_func(service->url);
	__vm_mem_free_func(service->username);
	__vm_mem_free_func(service->password);

	if (NULL != service->contents)
		__vm_mem_free_func(service->contents);

	vmware_data_shared_free(service->data);

	zbx_hashset_iter_reset(&service->entities, &iter);
	while (NULL != (entity = zbx_hashset_iter_next(&iter)))
		vmware_shared_perf_entity_clean(entity);

	zbx_hashset_destroy(&service->entities);

	zbx_hashset_iter_reset(&service->counters, &iter);
	while (NULL != (counter = zbx_hashset_iter_next(&iter)))
		vmware_counter_shared_clean(counter);

	zbx_hashset_destroy(&service->counters);

	__vm_mem_free_func(service);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_cluster_shared_dup                                        *
 *                                                                            *
 * Purpose: copies vmware cluster object into shared memory                   *
 *                                                                            *
 * Parameters: src   - [IN] the vmware cluster object                         *
 *                                                                            *
 * Return value: a copied vmware cluster object                               *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_cluster_t	*vmware_cluster_shared_dup(const zbx_vmware_cluster_t *src)
{
	zbx_vmware_cluster_t	*cluster;

	cluster = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_cluster_t));
	cluster->id = vmware_shared_strdup(src->id);
	cluster->name = vmware_shared_strdup(src->name);
	cluster->status = vmware_shared_strdup(src->status);

	return cluster;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_datastore_shared_dup                                      *
 *                                                                            *
 * Purpose: copies vmware hypervisor datastore object into shared memory      *
 *                                                                            *
 * Parameters: src   - [IN] the vmware datastore object                       *
 *                                                                            *
 * Return value: a duplicated vmware datastore object                         *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_datastore_t	*vmware_datastore_shared_dup(const zbx_vmware_datastore_t *src)
{
	zbx_vmware_datastore_t	*datastore;

	datastore = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_datastore_t));
	datastore->uuid = vmware_shared_strdup(src->uuid);
	datastore->name = vmware_shared_strdup(src->name);

	return datastore;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_dev_shared_dup                                            *
 *                                                                            *
 * Purpose: copies vmware virtual machine device object into shared memory    *
 *                                                                            *
 * Parameters: src   - [IN] the vmware device object                          *
 *                                                                            *
 * Return value: a duplicated vmware device object                            *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_dev_t	*vmware_dev_shared_dup(const zbx_vmware_dev_t *src)
{
	zbx_vmware_dev_t	*dev;

	dev = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_dev_t));
	dev->type = src->type;
	dev->instance = vmware_shared_strdup(src->instance);
	dev->label = vmware_shared_strdup(src->label);

	return dev;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_vm_shared_dup                                             *
 *                                                                            *
 * Purpose: copies vmware virtual machine object into shared memory           *
 *                                                                            *
 * Parameters: src   - [IN] the vmware virtual machine object                 *
 *                                                                            *
 * Return value: a duplicated vmware virtual machine object                   *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_vm_t	*vmware_vm_shared_dup(const zbx_vmware_vm_t *src)
{
	zbx_vmware_vm_t	*vm;
	int		i;

	vm = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_vm_t));

	VMWARE_VECTOR_CREATE(&vm->devs, ptr);

	vm->uuid = vmware_shared_strdup(src->uuid);
	vm->id = vmware_shared_strdup(src->id);
	vm->details = vmware_shared_strdup(src->details);

	for (i = 0; i < src->devs.values_num; i++)
		zbx_vector_ptr_append(&vm->devs, vmware_dev_shared_dup(src->devs.values[i]));

	return vm;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_hv_shared_dup                                             *
 *                                                                            *
 * Purpose: copies vmware hypervisor object into shared memory                *
 *                                                                            *
 * Parameters: src   - [IN] the vmware hypervisor object                      *
 *                                                                            *
 * Return value: a duplicated vmware hypervisor object                        *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_hv_t	*vmware_hv_shared_dup(const zbx_vmware_hv_t *src)
{
	zbx_vmware_hv_t	*hv;
	int		i;

	hv = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_hv_t));

	VMWARE_VECTOR_CREATE(&hv->datastores, ptr);
	VMWARE_VECTOR_CREATE(&hv->vms, ptr);

	hv->uuid = vmware_shared_strdup(src->uuid);
	hv->id = vmware_shared_strdup(src->id);
	hv->details = vmware_shared_strdup(src->details);
	hv->clusterid = vmware_shared_strdup(src->clusterid);

	for (i = 0; i < src->datastores.values_num; i++)
		zbx_vector_ptr_append(&hv->datastores, vmware_datastore_shared_dup(src->datastores.values[i]));

	for (i = 0; i < src->vms.values_num; i++)
		zbx_vector_ptr_append(&hv->vms, vmware_vm_shared_dup(src->vms.values[i]));

	return hv;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_data_shared_dup                                           *
 *                                                                            *
 * Purpose: copies vmware data object into shared memory                      *
 *                                                                            *
 * Parameters: src   - [IN] the vmware data object                            *
 *                                                                            *
 * Return value: a duplicated vmware data object                              *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_data_t	*vmware_data_shared_dup(const zbx_vmware_data_t *src)
{
	zbx_vmware_data_t	*data;
	int			i;

	data = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_data_t));

	VMWARE_VECTOR_CREATE(&data->hvs, ptr);
	VMWARE_VECTOR_CREATE(&data->clusters, ptr);

	data->events = vmware_shared_strdup(src->events);
	data->error =  vmware_shared_strdup(src->error);

	for (i = 0; i < src->clusters.values_num; i++)
		zbx_vector_ptr_append(&data->clusters, vmware_cluster_shared_dup(src->clusters.values[i]));

	for (i = 0; i < src->hvs.values_num; i++)
		zbx_vector_ptr_append(&data->hvs, vmware_hv_shared_dup(src->hvs.values[i]));

	return data;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_datastore_free                                            *
 *                                                                            *
 * Purpose: frees resources allocated to store datastore data                 *
 *                                                                            *
 * Parameters: datastore   - [IN] the datastore                               *
 *                                                                            *
 ******************************************************************************/
static void	vmware_datastore_free(zbx_vmware_datastore_t *datastore)
{
	zbx_free(datastore->name);
	zbx_free(datastore->uuid);
	zbx_free(datastore);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_dev_free                                                  *
 *                                                                            *
 * Purpose: frees resources allocated to store vm device data                 *
 *                                                                            *
 * Parameters: dev   - [IN] the vm device                                     *
 *                                                                            *
 ******************************************************************************/
static void	vmware_dev_free(zbx_vmware_dev_t *dev)
{
	zbx_free(dev->instance);
	zbx_free(dev->label);
	zbx_free(dev);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_vm_free                                                   *
 *                                                                            *
 * Purpose: frees resources allocated to store virtual machine                *
 *                                                                            *
 * Parameters: vm   - [IN] the virtual machine                                *
 *                                                                            *
 ******************************************************************************/
static void	vmware_vm_free(zbx_vmware_vm_t *vm)
{
	zbx_vector_ptr_clear_ext(&vm->devs, (zbx_clean_func_t)vmware_dev_free);
	zbx_vector_ptr_destroy(&vm->devs);

	zbx_free(vm->uuid);
	zbx_free(vm->id);
	zbx_free(vm->details);
	zbx_free(vm);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_hv_free                                                   *
 *                                                                            *
 * Purpose: frees resources allocated to store vmware hypervisor              *
 *                                                                            *
 * Parameters: hv   - [IN] the vmware hypervisor                              *
 *                                                                            *
 ******************************************************************************/
static void	vmware_hv_free(zbx_vmware_hv_t *hv)
{
	zbx_vector_ptr_clear_ext(&hv->datastores, (zbx_clean_func_t)vmware_datastore_free);
	zbx_vector_ptr_destroy(&hv->datastores);

	zbx_vector_ptr_clear_ext(&hv->vms, (zbx_clean_func_t)vmware_vm_free);
	zbx_vector_ptr_destroy(&hv->vms);

	zbx_free(hv->uuid);
	zbx_free(hv->id);
	zbx_free(hv->details);
	zbx_free(hv->clusterid);
	zbx_free(hv);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_cluster_free                                              *
 *                                                                            *
 * Purpose: frees resources allocated to store vmware cluster                 *
 *                                                                            *
 * Parameters: cluster   - [IN] the vmware cluster                            *
 *                                                                            *
 ******************************************************************************/
static void	vmware_cluster_free(zbx_vmware_cluster_t *cluster)
{
	zbx_free(cluster->name);
	zbx_free(cluster->id);
	zbx_free(cluster->status);
	zbx_free(cluster);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_data_free                                                 *
 *                                                                            *
 * Purpose: frees resources allocated to store vmware service data            *
 *                                                                            *
 * Parameters: data   - [IN] the vmware service data                          *
 *                                                                            *
 ******************************************************************************/
static void	vmware_data_free(zbx_vmware_data_t *data)
{
	zbx_vector_ptr_clear_ext(&data->hvs, (zbx_clean_func_t)vmware_hv_free);
	zbx_vector_ptr_destroy(&data->hvs);

	zbx_vector_ptr_clear_ext(&data->clusters, (zbx_clean_func_t)vmware_cluster_free);
	zbx_vector_ptr_destroy(&data->clusters);

	zbx_free(data->events);
	zbx_free(data->error);
	zbx_free(data);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_perf_entity_free                                          *
 *                                                                            *
 * Purpose: frees vmware peformance entity and the resources allocated by it  *
 *                                                                            *
 * Parameters: entity - [IN] the entity to free                               *
 *                                                                            *
 ******************************************************************************/
static void	vmware_perf_entity_free(zbx_vmware_perf_entity_t *entity)
{
	/* entities allocated on heap do not use counters vector */
	zbx_free(entity->type);
	zbx_free(entity->id);
	zbx_free(entity);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_counter_free                                              *
 *                                                                            *
 * Purpose: frees vmware performance counter and the resources allocated by   *
 *          it                                                                *
 *                                                                            *
 * Parameters: counter - [IN] the performance counter to free                 *
 *                                                                            *
 ******************************************************************************/
static void	vmware_counter_free(zbx_vmware_counter_t *counter)
{
	zbx_free(counter->path);
	zbx_free(counter);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_authenticate                                      *
 *                                                                            *
 * Purpose: authenticates vmware service                                      *
 *                                                                            *
 * Parameters: service    - [IN] the vmware service                           *
 *             easyhandle - [IN] the CURL handle                              *
 *             error      - [OUT] the error message in the case of failure    *
 *                                                                            *
 * Return value: SUCCEED - the authentication was completed successfully      *
 *               FAIL    - the authentication process has failed              *
 *                                                                            *
 * Comments: If service type is unknown this function will attempt to         *
 *           determine the right service type by trying to login with vCenter *
 *           and vSphere session managers.                                    *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_authenticate(zbx_vmware_service_t *service, CURL *easyhandle, char **error)
{
#	define ZBX_POST_VMWARE_AUTH						\
		ZBX_POST_VSPHERE_HEADER						\
		"<ns0:Login xsi:type=\"ns0:LoginRequestType\">"			\
			"<ns0:_this type=\"SessionManager\">%s</ns0:_this>"	\
			"<ns0:userName>%s</ns0:userName>"			\
			"<ns0:password>%s</ns0:password>"			\
		"</ns0:Login>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_authenticate";
	char		xml[MAX_STRING_LEN], *error_object = NULL;
	int		err, opt, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() '%s'@'%s'", __function_name, service->username, service->url);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_COOKIEFILE, "")) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_FOLLOWLOCATION, 1L)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_WRITEFUNCTION, curl_write_cb)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_HEADERFUNCTION, curl_header_cb)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_SSL_VERIFYPEER, 0L)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POST, 1L)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_URL, service->url)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_TIMEOUT,
					(long)CONFIG_VMWARE_TIMEOUT)) ||
			CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_SSL_VERIFYHOST, 0L)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	if (NULL != CONFIG_SOURCE_IP)
	{
		if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_INTERFACE, CONFIG_SOURCE_IP)))
		{
			*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
			goto out;
		}
	}

	if (ZBX_VMWARE_TYPE_UNKNOWN == service->type)
	{
		/* try to detect the service type first using vCenter service manager object */
		zbx_snprintf(xml, sizeof(xml), ZBX_POST_VMWARE_AUTH,
				vmware_service_objects[ZBX_VMWARE_TYPE_VCENTER].session_manager,
				service->username, service->password);

		if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, xml)))
		{
			*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
			goto out;
		}

		page.offset = 0;

		if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
		{
			*error = zbx_strdup(*error, curl_easy_strerror(err));
			goto out;
		}

		zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

		if (NULL == (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		{
			/* Successfully authenticated with vcenter service manager. */
			/* Set the service type and return with success.            */
			service->type = ZBX_VMWARE_TYPE_VCENTER;
			ret = SUCCEED;
			goto out;
		}

		/* If the wrong service manager was used, set the service type as vsphere and */
		/* try again with vsphere service manager. Otherwise return with failure.     */
		if (NULL == (error_object = zbx_xml_read_value(page.data,
				ZBX_XPATH_LN3("detail", "NotAuthenticatedFault", "object"))))
		{
			goto out;
		}

		if (0 != strcmp(error_object, vmware_service_objects[ZBX_VMWARE_TYPE_VCENTER].session_manager))
			goto out;

		service->type = ZBX_VMWARE_TYPE_VSPHERE;
		zbx_free(*error);
	}

	zbx_snprintf(xml, sizeof(xml), ZBX_POST_VMWARE_AUTH, vmware_service_objects[service->type].session_manager,
			service->username, service->password);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, xml)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	ret = SUCCEED;
out:
	zbx_free(error_object);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_contents                                      *
 *                                                                            *
 * Purpose: retrieves vmware service instance contents                        *
 *                                                                            *
 * Parameters: service    - [IN] the vmware service                           *
 *             easyhandle - [IN] the CURL handle                              *
 *             error      - [OUT] the error message in the case of failure    *
 *                                                                            *
 * Return value: SUCCEED - the contents were retrieved successfully           *
 *               FAIL    - the content retrieval faield                       *
 *                                                                            *
 ******************************************************************************/
static	int	vmware_service_get_contents(zbx_vmware_service_t *service, CURL *easyhandle, char **contents,
		char **error)
{
#	define ZBX_POST_VMWARE_CONTENTS 							\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrieveServiceContent>"							\
			"<ns0:_this type=\"ServiceInstance\">ServiceInstance</ns0:_this>"	\
		"</ns0:RetrieveServiceContent>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_contents";

	int		err, opt, ret = FAIL;

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, ZBX_POST_VMWARE_CONTENTS)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	*contents = zbx_strdup(*contents, page.data);

	ret = SUCCEED;
out:
	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_perf_counter_refreshrate                      *
 *                                                                            *
 * Purpose: get the performance counter refreshrate for the specified entity  *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             type         - [IN] the entity type (HostSystem or             *
 *                                 VirtualMachine)                            *
 *             id           - [IN] the entity id                              *
 *             refresh_rate - [OUT] a pointer to variable to store the        *
 *                                  regresh rate                              *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the authentication was completed successfully      *
 *               FAIL    - the authentication process has failed              *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_perf_counter_refreshrate(const zbx_vmware_service_t *service, CURL *easyhandle,
		const char *type, const char *id, int *refresh_rate, char **error)
{
#	define ZBX_POST_VCENTER_PERF_COUNTERS_REFRESH_RATE			\
		ZBX_POST_VSPHERE_HEADER						\
		"<ns0:QueryPerfProviderSummary>"				\
			"<ns0:_this type=\"PerformanceManager\">%s</ns0:_this>"	\
			"<ns0:entity type=\"%s\">%s</ns0:entity>"		\
		"</ns0:QueryPerfProviderSummary>"				\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_perfcounter_refreshrate";

	char		tmp[MAX_STRING_LEN], *value = NULL;
	int		err, opt, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VCENTER_PERF_COUNTERS_REFRESH_RATE,
			vmware_service_objects[service->type].performance_manager, type, id);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	if (NULL == (value = zbx_xml_read_value(page.data, ZBX_XPATH_REFRESHRATE())))
	{
		*error = zbx_strdup(*error, "Cannot find refreshRate.");
		goto out;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "%s() refresh_rate:%s", __function_name, value);

	if (SUCCEED != (ret = is_uint31(value, refresh_rate)))
		*error = zbx_dsprintf(*error, "Cannot convert refreshRate from %s.",  value);

	zbx_free(value);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_perf_counters                                 *
 *                                                                            *
 * Purpose: get the performance counter ids                                   *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_perf_counters(zbx_vmware_service_t *service, CURL *easyhandle,
		zbx_vector_ptr_t *counters, char **error)
{
#	define ZBX_POST_VMWARE_GET_PERFCOUTNER							\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrieveProperties>"							\
			"<ns0:_this type=\"PropertyCollector\">%s</ns0:_this>"			\
			"<ns0:specSet>"								\
				"<ns0:propSet>"							\
					"<ns0:type>PerformanceManager</ns0:type>"		\
					"<ns0:pathSet>perfCounter</ns0:pathSet>"		\
				"</ns0:propSet>"						\
				"<ns0:objectSet>"						\
					"<ns0:obj type=\"PerformanceManager\">%s</ns0:obj>"	\
				"</ns0:objectSet>"						\
			"</ns0:specSet>"							\
		"</ns0:RetrieveProperties>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_perfcounters";
	char		tmp[MAX_STRING_LEN], *group = NULL, *key = NULL, *rollup = NULL,
			*counterid = NULL;
	xmlDoc		*doc;
	xmlXPathContext	*xpathCtx;
	xmlXPathObject	*xpathObj;
	xmlNodeSetPtr	nodeset;
	int		opts, err, i, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VMWARE_GET_PERFCOUTNER,
			vmware_service_objects[service->type].property_collector,
			vmware_service_objects[service->type].performance_manager);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opts = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opts, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;


	if (NULL == (doc = xmlReadMemory(page.data, page.offset, ZBX_VM_NONAME_XML, NULL, 0)))
	{
		*error = zbx_strdup(*error, "Cannot parse performance counter list.");
		goto out;
	}

	xpathCtx = xmlXPathNewContext(doc);

	if (NULL == (xpathObj = xmlXPathEvalExpression((xmlChar *)ZBX_XPATH_COUNTERINFO(), xpathCtx)))
	{
		*error = zbx_strdup(*error, "Cannot make performance counter list parsing query.");
		goto clean;
	}

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
	{
		*error = zbx_strdup(*error, "Cannot find items in performance counter list.");
		goto clean;
	}

	nodeset = xpathObj->nodesetval;

	for (i = 0; i < nodeset->nodeNr; i++)
	{
		zbx_vmware_counter_t	*counter;

		group = zbx_xml_read_node_value(doc, nodeset->nodeTab[i],
				"*[local-name()='groupInfo']/*[local-name()='key']");

		key = zbx_xml_read_node_value(doc, nodeset->nodeTab[i],
						"*[local-name()='nameInfo']/*[local-name()='key']");

		rollup = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='rollupType']");
		counterid = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='key']");

		if (NULL != group && NULL != key && NULL != rollup && NULL != counterid)
		{
			counter = zbx_malloc(NULL, sizeof(zbx_vmware_counter_t));
			counter->path = zbx_dsprintf(NULL, "%s/%s[%s]", group, key, rollup);
			ZBX_STR2UINT64(counter->id, counterid);

			zbx_vector_ptr_append(counters, counter);

			zabbix_log(LOG_LEVEL_DEBUG, "adding performance counter %s:" ZBX_FS_UI64, counter->path,
					counter->id);
		}

		zbx_free(counterid);
		zbx_free(rollup);
		zbx_free(key);
		zbx_free(group);
	}

	/* The counter data uses a lot of memory which is needed only once during initialization. */
	/* Reset the download buffer afterwards so the memory is not wasted.                      */
	zbx_free(page.data);
	page.alloc = 0;
	page.offset = 0;

	ret = SUCCEED;
clean:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: wmware_vm_get_nic_devices                                        *
 *                                                                            *
 * Purpose: gets virtual machine network interface devices                    *
 *                                                                            *
 * Parameters: vm     - [IN] the virtual machine                              *
 *                                                                            *
 * Comments: The network interface devices are taken from vm device list      *
 *           filtered by macAddress key.                                      *
 *                                                                            *
 ******************************************************************************/
static void	wmware_vm_get_nic_devices(zbx_vmware_vm_t *vm)
{
	const char	*__function_name = "wmware_vm_get_nic_devices";

	xmlDoc		*doc;
	xmlXPathContext	*xpathCtx;
	xmlXPathObject	*xpathObj;
	xmlNodeSetPtr	nodeset;
	int		i, nics = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL == (doc = xmlReadMemory(vm->details, strlen(vm->details), ZBX_VM_NONAME_XML, NULL, 0)))
		goto out;

	xpathCtx = xmlXPathNewContext(doc);

	if (NULL == (xpathObj = xmlXPathEvalExpression((xmlChar *)ZBX_XPATH_VM_HARDWARE("device")
			"[*[local-name()='macAddress']]", xpathCtx)))
	{
		goto clean;
	}

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto clean;

	nodeset = xpathObj->nodesetval;

	for (i = 0; i < nodeset->nodeNr; i++)
	{
		char			*key;
		zbx_vmware_dev_t	*dev;

		if (NULL == (key = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='key']")))
			continue;

		dev = zbx_malloc(NULL, sizeof(zbx_vmware_dev_t));
		dev->type =  ZBX_VMWARE_DEV_TYPE_NIC;
		dev->instance = zbx_strdup(NULL, key);
		dev->label = zbx_xml_read_node_value(doc, nodeset->nodeTab[i],
				"*[local-name()='deviceInfo']/*[local-name()='label']");

		zbx_vector_ptr_append(&vm->devs, dev);
		nics++;

		zbx_free(key);
	}
clean:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() found:%d", __function_name, nics);
}

/******************************************************************************
 *                                                                            *
 * Function: wmware_vm_get_disk_devices                                       *
 *                                                                            *
 * Purpose: gets virtual machine virtual disk devices                         *
 *                                                                            *
 * Parameters: vm     - [IN] the virtual machine                              *
 *                                                                            *
 ******************************************************************************/
static void	wmware_vm_get_disk_devices(zbx_vmware_vm_t *vm)
{
	const char	*__function_name = "wmware_vm_get_disk_devices";

	xmlDoc		*doc;
	xmlXPathContext	*xpathCtx;
	xmlXPathObject	*xpathObj;
	xmlNodeSetPtr	nodeset;
	int		i, disks = 0;
	char		*xpath = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL == (doc = xmlReadMemory(vm->details, strlen(vm->details), ZBX_VM_NONAME_XML, NULL, 0)))
		goto out;

	xpathCtx = xmlXPathNewContext(doc);

	/* select all hardware devices of VirtualDisk type */
	if (NULL == (xpathObj = xmlXPathEvalExpression((xmlChar *)ZBX_XPATH_VM_HARDWARE("device")
			"[string(@*[local-name()='type'])='VirtualDisk']", xpathCtx)))
	{
		goto clean;
	}

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto clean;

	nodeset = xpathObj->nodesetval;

	for (i = 0; i < nodeset->nodeNr; i++)
	{
		zbx_vmware_dev_t	*dev;
		char			*unitNumber = NULL, *controllerKey = NULL, *busNumber = NULL,
					*scsiCtlrUnitNumber = NULL;
		xmlXPathObject		*xpathObjController = NULL;

		do
		{
			if (NULL == (unitNumber = zbx_xml_read_node_value(doc, nodeset->nodeTab[i],
					"*[local-name()='unitNumber']")))
			{
				break;
			}

			if (NULL == (controllerKey = zbx_xml_read_node_value(doc, nodeset->nodeTab[i],
					"*[local-name()='controllerKey']")))
			{
				break;
			}

			/* find the controller (parent) device */
			xpath = zbx_dsprintf(xpath, ZBX_XPATH_VM_HARDWARE("device")
					"[*[local-name()='key']/text()='%s']", controllerKey);

			if (NULL == (xpathObjController = xmlXPathEvalExpression((xmlChar *)xpath, xpathCtx)))
				break;

			if (0 != xmlXPathNodeSetIsEmpty(xpathObjController->nodesetval))
				break;

			if (NULL == (busNumber = zbx_xml_read_node_value(doc,
					xpathObjController->nodesetval->nodeTab[0], "*[local-name()='busNumber']")))
			{
				break;
			}

			/* scsiCtlrUnitNumber property is simply used to determine controller type. */
			/* For IDE controllers it is not set.                                       */
			scsiCtlrUnitNumber = zbx_xml_read_node_value(doc, xpathObjController->nodesetval->nodeTab[0],
				"*[local-name()='scsiCtlrUnitNumber']");

			dev = zbx_malloc(NULL, sizeof(zbx_vmware_dev_t));
			dev->type =  ZBX_VMWARE_DEV_TYPE_DISK;

			/* the virtual disk instance has format <controller type><busNumber>:<unitNumber> */
			/* where controller type is either ide or scsi depending on the controller type   */
			dev->instance = zbx_dsprintf(NULL, "%s%s:%s", (NULL == scsiCtlrUnitNumber ? "ide" : "scsi"),
					busNumber, unitNumber);

			dev->label = zbx_xml_read_node_value(doc, nodeset->nodeTab[i],
					"*[local-name()='deviceInfo']/*[local-name()='label']");

			zbx_vector_ptr_append(&vm->devs, dev);

			disks++;

		}
		while (0);

		if (NULL != xpathObjController)
			xmlXPathFreeObject(xpathObjController);

		zbx_free(scsiCtlrUnitNumber);
		zbx_free(busNumber);
		zbx_free(unitNumber);
		zbx_free(controllerKey);

	}
clean:
	zbx_free(xpath);

	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() found:%d", __function_name, disks);

}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_vm_data                                       *
 *                                                                            *
 * Purpose: gets the virtual machine data                                     *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             vmid         - [IN] the virtual machine id                     *
 *             data         - [OUT] a reference to output variable            *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_vm_data(zbx_vmware_service_t *service, CURL *easyhandle, const char *vmid,
		char **data, char **error)
{
#	define ZBX_POST_VMWARE_VM_STATUS_EX 						\
		ZBX_POST_VSPHERE_HEADER							\
		"<ns0:RetrievePropertiesEx>"						\
			"<ns0:_this type=\"PropertyCollector\">%s</ns0:_this>"		\
			"<ns0:specSet>"							\
				"<ns0:propSet>"						\
					"<ns0:type>VirtualMachine</ns0:type>"		\
					"<ns0:pathSet>config.hardware</ns0:pathSet>"	\
					"<ns0:pathSet>config.uuid</ns0:pathSet>"	\
					"<ns0:pathSet>config.instanceUuid</ns0:pathSet>"\
					"<ns0:pathSet>summary</ns0:pathSet>"		\
					"<ns0:pathSet>guest</ns0:pathSet>"		\
				"</ns0:propSet>"					\
				"<ns0:objectSet>"					\
					"<ns0:obj type=\"VirtualMachine\">%s</ns0:obj>"	\
				"</ns0:objectSet>"					\
			"</ns0:specSet>"						\
			"<ns0:options></ns0:options>"					\
		"</ns0:RetrievePropertiesEx>"						\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_vm_data";

	char		tmp[MAX_STRING_LEN];
	int		err, opt, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() vmid:'%s'", __function_name, vmid);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VMWARE_VM_STATUS_EX,
			vmware_service_objects[service->type].property_collector, vmid);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	*data = zbx_strdup(NULL, page.data);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_create_vm                                         *
 *                                                                            *
 * Purpose: create virtual machine object                                     *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             id           - [IN] the virtual machine id                     *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: The created virtual machine object or NULL if an error was   *
 *               detected.                                                    *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_vm_t	*vmware_service_create_vm(zbx_vmware_service_t *service,  CURL *easyhandle,
		const char *id, char **error)
{
	const char	*__function_name = "vmware_service_create_vm";

	zbx_vmware_vm_t	*vm;
	char		*value;
	const char	*uuid_xpath[3] = {NULL, ZBX_XPATH_VM_UUID(), ZBX_XPATH_VM_INSTANCE_UUID()};
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() vmid:'%s'", __function_name, id);

	vm = zbx_malloc(NULL, sizeof(zbx_vmware_vm_t));
	memset(vm, 0, sizeof(zbx_vmware_vm_t));

	zbx_vector_ptr_create(&vm->devs);

	if (SUCCEED != vmware_service_get_vm_data(service, easyhandle, id, &vm->details, error))
		goto out;

	if (NULL == (value = zbx_xml_read_value(vm->details, uuid_xpath[service->type])))
		goto out;

	vm->uuid = value;
	vm->id = zbx_strdup(NULL, id);

	wmware_vm_get_nic_devices(vm);
	wmware_vm_get_disk_devices(vm);

	ret = SUCCEED;
out:
	if (SUCCEED != ret)
	{
		vmware_vm_free(vm);
		vm = NULL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return vm;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_create_datastore                                  *
 *                                                                            *
 * Purpose: create vmware hypervisor datastore object                         *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             id           - [IN] the datastore id                           *
 *                                                                            *
 * Return value: The created datastore object or NULL if an error was         *
 *                detected                                                    *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_datastore_t	*vmware_service_create_datastore(const zbx_vmware_service_t *service, CURL *easyhandle,
		const char *id)
{
#	define ZBX_POST_DATASTORE_GET								\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrievePropertiesEx>"							\
			"<ns0:_this type=\"PropertyCollector\">%s</ns0:_this>"			\
			"<ns0:specSet>"								\
				"<ns0:propSet>"							\
					"<ns0:type>Datastore</ns0:type>"			\
					"<ns0:pathSet>summary</ns0:pathSet>"			\
				"</ns0:propSet>"						\
				"<ns0:objectSet>"						\
					"<ns0:obj type=\"Datastore\">%s</ns0:obj>"		\
				"</ns0:objectSet>"						\
			"</ns0:specSet>"							\
			"<ns0:options/>"							\
		"</ns0:RetrievePropertiesEx>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char		*__function_name = "vmware_service_create_datastore";

	char			tmp[MAX_STRING_LEN], *uuid = NULL, *name = NULL, *url;
	zbx_vmware_datastore_t	*datastore = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() datastore:'%s'", __function_name, id);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_DATASTORE_GET,
			vmware_service_objects[service->type].property_collector, id);

	if (CURLE_OK != curl_easy_setopt(easyhandle, CURLOPT_POSTFIELDS, tmp))
		goto out;

	page.offset = 0;

	if (CURLE_OK != curl_easy_perform(easyhandle))
		goto out;

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	name = zbx_xml_read_value(page.data, ZBX_XPATH_DATASTORE("name"));

	if (NULL != (url = zbx_xml_read_value(page.data, ZBX_XPATH_DATASTORE("url"))))
	{
		if ('\0' != *url)
		{
			size_t	len;
			char	*ptr;

			len = strlen(url);

			if ('/' == url[len - 1])
				url[len - 1] = '\0';

			for (ptr = url + len - 2; ptr > url && *ptr != '/'; ptr--)
				;

			uuid = zbx_strdup(NULL, ptr + 1);
		}
		zbx_free(url);
	}
out:
	datastore = zbx_malloc(NULL, sizeof(zbx_vmware_datastore_t));
	datastore->name = (NULL != name) ? name : zbx_strdup(NULL, id);
	datastore->uuid = uuid;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return datastore;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_hv_data                                       *
 *                                                                            *
 * Purpose: gets the vmware hypervisor data                                   *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             hvid         - [IN] the vmware hypervisor id                   *
 *             data         - [OUT] a reference to output variable            *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_hv_data(const zbx_vmware_service_t *service, CURL *easyhandle, const char *hvid,
		char **data, char **error)
{
#	define ZBX_POST_hv_DETAILS 								\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrieveProperties>"							\
			"<ns0:_this type=\"PropertyCollector\">%s</ns0:_this>"			\
			"<ns0:specSet>"								\
				"<ns0:propSet>"							\
					"<ns0:type>HostSystem</ns0:type>"			\
					"<ns0:pathSet>name</ns0:pathSet>"			\
					"<ns0:pathSet>vm</ns0:pathSet>"				\
					"<ns0:pathSet>summary.quickStats</ns0:pathSet>"		\
					"<ns0:pathSet>summary.config</ns0:pathSet>"		\
					"<ns0:pathSet>summary.hardware</ns0:pathSet>"		\
					"<ns0:pathSet>parent</ns0:pathSet>"			\
					"<ns0:pathSet>datastore</ns0:pathSet>"			\
					"<ns0:pathSet>runtime.healthSystemRuntime."		\
							"systemHealthInfo</ns0:pathSet>"	\
				"</ns0:propSet>"						\
				"<ns0:objectSet>"						\
					"<ns0:obj type=\"HostSystem\">%s</ns0:obj>"		\
				"</ns0:objectSet>"						\
			"</ns0:specSet>"							\
		"</ns0:RetrieveProperties>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_hv_data";

	char		tmp[MAX_STRING_LEN];
	int		err, opt, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() guesthvid:'%s'", __function_name, hvid);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_hv_DETAILS,
			vmware_service_objects[service->type].property_collector, hvid);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	*data = zbx_strdup(NULL, page.data);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_create_hv                                         *
 *                                                                            *
 * Purpose: create vmware hypervisor object                                   *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             id           - [IN] the vmware hypervisor id                   *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: The created vmware hypervisor object or NULL if an error was *
 *               detected.                                                    *
 *                                                                            *
 ******************************************************************************/
static zbx_vmware_hv_t	*vmware_service_create_hv(zbx_vmware_service_t *service, CURL *easyhandle,
		const char *id, char **error)
{
	const char		*__function_name = "vmware_service_create_hv";

	zbx_vmware_hv_t		*hv;
	char			*value;
	zbx_vector_str_t	datastores, vms;
	int			i, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() hvid:'%s'", __function_name, id);

	hv = zbx_malloc(NULL, sizeof(zbx_vmware_hv_t));
	memset(hv, 0, sizeof(zbx_vmware_hv_t));

	zbx_vector_ptr_create(&hv->datastores);
	zbx_vector_ptr_create(&hv->vms);

	zbx_vector_str_create(&datastores);
	zbx_vector_str_create(&vms);

	if (SUCCEED != vmware_service_get_hv_data(service, easyhandle, id, &hv->details, error))
		goto out;

	if (NULL == (value = zbx_xml_read_value(hv->details, ZBX_XPATH_HV_HARDWARE("uuid"))))
		goto out;

	hv->uuid = value;
	hv->id = zbx_strdup(NULL, id);

	if (NULL != (value = zbx_xml_read_value(hv->details, "//*[@type='ClusterComputeResource']")))
	{
		hv->clusterid = value;
	}

	zbx_xml_read_values(hv->details, ZBX_XPATH_HV_DATASTORES(), &datastores);

	for (i = 0; i < datastores.values_num; i++)
	{
		zbx_vmware_datastore_t	*datastore;

		if (NULL != (datastore = vmware_service_create_datastore(service, easyhandle, datastores.values[i])))
			zbx_vector_ptr_append(&hv->datastores, datastore);
	}

	zbx_xml_read_values(hv->details, ZBX_XPATH_HV_VMS(), &vms);

	for (i = 0; i < vms.values_num; i++)
	{
		zbx_vmware_vm_t	*vm;

		if (NULL != (vm = vmware_service_create_vm(service, easyhandle, vms.values[i], error)))
			zbx_vector_ptr_append(&hv->vms, vm);
	}

	ret = SUCCEED;
out:
	zbx_vector_str_clear_ext(&vms, zbx_ptr_free);
	zbx_vector_str_destroy(&vms);

	zbx_vector_str_clear_ext(&datastores, zbx_ptr_free);
	zbx_vector_str_destroy(&datastores);

	if (SUCCEED != ret)
	{
		vmware_hv_free(hv);
		hv = NULL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return hv;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_hv_list                                       *
 *                                                                            *
 * Purpose: retrieves a list of all vmware service hypervisor ids             *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             hvs          - [OUT] list of vmware hypervisor ids             *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_hv_list(const zbx_vmware_service_t *service, CURL *easyhandle,
		zbx_vector_str_t *hvs, char **error)
{
#	define ZBX_POST_VCENTER_HV_LIST								\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrievePropertiesEx xsi:type=\"ns0:RetrievePropertiesExRequestType\">"	\
			"<ns0:_this type=\"PropertyCollector\">propertyCollector</ns0:_this>"	\
			"<ns0:specSet>"								\
				"<ns0:propSet>"							\
					"<ns0:type>HostSystem</ns0:type>"			\
				"</ns0:propSet>"						\
				"<ns0:objectSet>"						\
					"<ns0:obj type=\"Folder\">group-d1</ns0:obj>"		\
					"<ns0:skip>false</ns0:skip>"				\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>visitFolders</ns0:name>"		\
						"<ns0:type>Folder</ns0:type>"			\
						"<ns0:path>childEntity</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>dcToHf</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>dcToVmf</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>crToH</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>crToRp</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>dcToDs</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>hToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>dcToVmf</ns0:name>"			\
						"<ns0:type>Datacenter</ns0:type>"		\
						"<ns0:path>vmFolder</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>dcToDs</ns0:name>"			\
						"<ns0:type>Datacenter</ns0:type>"		\
						"<ns0:path>datastore</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>dcToHf</ns0:name>"			\
						"<ns0:type>Datacenter</ns0:type>"		\
						"<ns0:path>hostFolder</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>crToH</ns0:name>"			\
						"<ns0:type>ComputeResource</ns0:type>"		\
						"<ns0:path>host</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>crToRp</ns0:name>"			\
						"<ns0:type>ComputeResource</ns0:type>"		\
						"<ns0:path>resourcePool</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToRp</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>rpToRp</ns0:name>"			\
						"<ns0:type>ResourcePool</ns0:type>"		\
						"<ns0:path>resourcePool</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToRp</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>hToVm</ns0:name>"			\
						"<ns0:type>HostSystem</ns0:type>"		\
						"<ns0:path>vm</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>rpToVm</ns0:name>"			\
						"<ns0:type>ResourcePool</ns0:type>"		\
						"<ns0:path>vm</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
					"</ns0:selectSet>"					\
				"</ns0:objectSet>"						\
			"</ns0:specSet>"							\
			"<ns0:options/>"							\
		"</ns0:RetrievePropertiesEx>"							\
		ZBX_POST_VSPHERE_FOOTER

#	define ZBX_POST_VCENTER_HV_LIST_CONTINUE								\
		ZBX_POST_VSPHERE_HEADER										\
		"<ns0:ContinueRetrievePropertiesEx xsi:type=\"ns0:ContinueRetrievePropertiesExRequestType\">"	\
			"<ns0:_this type=\"PropertyCollector\">propertyCollector</ns0:_this>"			\
			"<ns0:token>%s</ns0:token>"								\
		"</ns0:ContinueRetrievePropertiesEx>"								\
		ZBX_POST_VSPHERE_FOOTER

#	define ZBX_XPATH_RETRIEVE_PROPERTIES_TOKEN			\
		"/*[local-name()='Envelope']/*[local-name()='Body']"	\
		"/*[local-name()='RetrievePropertiesExResponse']"	\
		"/*[local-name()='returnval']/*[local-name()='token']"

#	define ZBX_XPATH_CONTINUE_RETRIEVE_PROPERTIES_TOKEN			\
		"/*[local-name()='Envelope']/*[local-name()='Body']"		\
		"/*[local-name()='ContinueRetrievePropertiesExResponse']"	\
		"/*[local-name()='returnval']/*[local-name()='token']"

	const char	*__function_name = "vmware_service_get_hv_list";

	char		tmp[MAX_STRING_LEN];
	int		err, opt, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (ZBX_VMWARE_TYPE_VCENTER == service->type)
	{
		char	*token, *token_xpath = NULL;

		if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, ZBX_POST_VCENTER_HV_LIST)))
		{
			*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
			goto out;
		}

		while (1)
		{
			page.offset = 0;

			if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
			{
				*error = zbx_strdup(*error, curl_easy_strerror(err));
				goto out;
			}

			zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

			if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
				goto out;

			zbx_xml_read_values(page.data, "//*[@type='HostSystem']", hvs);

			if (NULL == token_xpath)
				token_xpath = ZBX_XPATH_RETRIEVE_PROPERTIES_TOKEN;
			else
				token_xpath = ZBX_XPATH_CONTINUE_RETRIEVE_PROPERTIES_TOKEN;

			if (NULL == (token = zbx_xml_read_value(page.data, token_xpath)))
				break;

			zabbix_log(LOG_LEVEL_DEBUG, "%s() continue retrieving properties with token: '%s'",
					__function_name, token);

			zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VCENTER_HV_LIST_CONTINUE, token);
			zbx_free(token);

			if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
			{
				*error = zbx_dsprintf(*error, "Cannot set cURL option [%d]: %s", opt,
						curl_easy_strerror(err));
				goto out;
			}
		}
	}
	else
	{
		zbx_vector_str_append(hvs, zbx_strdup(NULL, "ha-host"));
	}

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s found:%d", __function_name, zbx_result_string(ret), hvs->values_num);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_event_session                                 *
 *                                                                            *
 * Purpose: retrieves event session name                                      *
 *                                                                            *
 * Parameters: service        - [IN] the vmware service                       *
 *             easyhandle     - [IN] the CURL handle                          *
 *             event_session  - [OUT] a pointer to the output variable        *
 *             error          - [OUT] the error message in the case of failure*
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_event_session(const zbx_vmware_service_t *service, CURL *easyhandle,
		char **event_session, char **error)
{
#	define ZBX_POST_VMWARE_CREATE_EVENT_COLLECTOR				\
		ZBX_POST_VSPHERE_HEADER						\
		"<ns0:CreateCollectorForEvents>"				\
			"<ns0:_this type=\"EventManager\">%s</ns0:_this>"	\
			"<ns0:filter/>"						\
		"</ns0:CreateCollectorForEvents>"				\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_event_session";

	char		tmp[MAX_STRING_LEN];
	int		err, opt, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VMWARE_CREATE_EVENT_COLLECTOR,
			vmware_service_objects[service->type].event_manager);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	if (NULL == (*event_session = zbx_xml_read_value(page.data, "/*/*/*/*[@type='EventHistoryCollector']")))
	{
		*error = zbx_strdup(*error, "Cannot get EventHistoryCollector session.");
		goto out;
	}

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_destroy_event_session                             *
 *                                                                            *
 * Purpose: destroys event session                                            *
 *                                                                            *
 * Parameters: service        - [IN] the vmware service                       *
 *             easyhandle     - [IN] the CURL handle                          *
 *             event_session  - [IN] event session (EventHistoryCollector)    *
 *                                   identifier                               *
 *             error          - [OUT] the error message in the case of failure*
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_destroy_event_session(const zbx_vmware_service_t *service, CURL *easyhandle,
		const char *event_session, char **error)
{
#	define ZBX_POST_VMWARE_DESTROY_EVENT_COLLECTOR					\
		ZBX_POST_VSPHERE_HEADER							\
		"<ns0:DestroyCollector>"						\
			"<ns0:_this type=\"EventHistoryCollector\">%s</ns0:_this>"	\
		"</ns0:DestroyCollector>"						\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_destroy_event_session";

	int		err, opt, ret = FAIL;
	char		tmp[MAX_STRING_LEN];

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VMWARE_DESTROY_EVENT_COLLECTOR, event_session);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_event_data                                    *
 *                                                                            *
 * Purpose: retrieves event data                                              *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             events       - [OUT] a pointer to the output variable          *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_event_data(const zbx_vmware_service_t *service, CURL *easyhandle, char **events,
		char **error)
{
#	define ZBX_POST_VMWARE_EVENTS_GET							\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrievePropertiesEx>"							\
			"<ns0:_this type=\"PropertyCollector\">%s</ns0:_this>"			\
			"<ns0:specSet>"								\
				"<ns0:propSet>"							\
					"<ns0:type>EventHistoryCollector</ns0:type>"		\
					"<ns0:all>true</ns0:all>"				\
				"</ns0:propSet>"						\
				"<ns0:objectSet>"						\
					"<ns0:obj type=\"EventHistoryCollector\">%s</ns0:obj>"	\
					"<ns0:skip>false</ns0:skip>"				\
				"</ns0:objectSet>"						\
			"</ns0:specSet>"							\
			"<ns0:options/>"							\
		"</ns0:RetrievePropertiesEx>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_event_data";

	char		tmp[MAX_STRING_LEN], *event_session = NULL;
	int		err, o, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() event_session:'%s'", __function_name, event_session);

	if (SUCCEED != vmware_service_get_event_session(service, easyhandle, &event_session, error))
		goto out;

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VMWARE_EVENTS_GET,
			vmware_service_objects[service->type].property_collector, event_session);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, o = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", o, curl_easy_strerror(err));
		goto end_session;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto end_session;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto end_session;

	*events = zbx_strdup(NULL, page.data);

	ret = SUCCEED;

end_session:
	if (SUCCEED != vmware_service_destroy_event_session(service, easyhandle, event_session, error))
		ret = FAIL;
out:
	zbx_free(event_session);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_clusters                                      *
 *                                                                            *
 * Purpose: retrieves a list of vmware service clusters                       *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             clusters     - [OUT] a pointer to the output variable          *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_clusters(const zbx_vmware_service_t *service, CURL *easyhandle, char **clusters,
		char **error)
{
#	define ZBX_POST_VCENTER_CLUSTER								\
		ZBX_POST_VSPHERE_HEADER								\
		"<ns0:RetrievePropertiesEx xsi:type=\"ns0:RetrievePropertiesExRequestType\">"	\
			"<ns0:_this type=\"PropertyCollector\">propertyCollector</ns0:_this>"	\
			"<ns0:specSet>"								\
				"<ns0:propSet>"							\
					"<ns0:type>ClusterComputeResource</ns0:type>"		\
					"<ns0:pathSet>name</ns0:pathSet>"			\
				"</ns0:propSet>"						\
				"<ns0:objectSet>"						\
					"<ns0:obj type=\"Folder\">group-d1</ns0:obj>"		\
					"<ns0:skip>false</ns0:skip>"				\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>visitFolders</ns0:name>"		\
						"<ns0:type>Folder</ns0:type>"			\
						"<ns0:path>childEntity</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>dcToHf</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>dcToVmf</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>crToH</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>crToRp</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>dcToDs</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>hToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>dcToVmf</ns0:name>"			\
						"<ns0:type>Datacenter</ns0:type>"		\
						"<ns0:path>vmFolder</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>dcToDs</ns0:name>"			\
						"<ns0:type>Datacenter</ns0:type>"		\
						"<ns0:path>datastore</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>dcToHf</ns0:name>"			\
						"<ns0:type>Datacenter</ns0:type>"		\
						"<ns0:path>hostFolder</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>crToH</ns0:name>"			\
						"<ns0:type>ComputeResource</ns0:type>"		\
						"<ns0:path>host</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>crToRp</ns0:name>"			\
						"<ns0:type>ComputeResource</ns0:type>"		\
						"<ns0:path>resourcePool</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToRp</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>rpToRp</ns0:name>"			\
						"<ns0:type>ResourcePool</ns0:type>"		\
						"<ns0:path>resourcePool</ns0:path>"		\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToRp</ns0:name>"		\
						"</ns0:selectSet>"				\
						"<ns0:selectSet>"				\
							"<ns0:name>rpToVm</ns0:name>"		\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>hToVm</ns0:name>"			\
						"<ns0:type>HostSystem</ns0:type>"		\
						"<ns0:path>vm</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
						"<ns0:selectSet>"				\
							"<ns0:name>visitFolders</ns0:name>"	\
						"</ns0:selectSet>"				\
					"</ns0:selectSet>"					\
					"<ns0:selectSet xsi:type=\"ns0:TraversalSpec\">"	\
						"<ns0:name>rpToVm</ns0:name>"			\
						"<ns0:type>ResourcePool</ns0:type>"		\
						"<ns0:path>vm</ns0:path>"			\
						"<ns0:skip>false</ns0:skip>"			\
					"</ns0:selectSet>"					\
				"</ns0:objectSet>"						\
			"</ns0:specSet>"							\
			"<ns0:options/>"							\
		"</ns0:RetrievePropertiesEx>"							\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_clusters";

	int		err, o, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, o = CURLOPT_POSTFIELDS, ZBX_POST_VCENTER_CLUSTER)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", o, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	*clusters = zbx_strdup(*clusters, page.data);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_cluster_status                                *
 *                                                                            *
 * Purpose: retrieves status of the specified vmware cluster                  *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             clusterid    - [IN] the cluster id                             *
 *             status       - [OUT] a pointer to the output variable          *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_cluster_status(const zbx_vmware_service_t *service, CURL *easyhandle,
		const char *clusterid, char **status, char **error)
{
#	define ZBX_POST_VMWARE_CLUSTER_STATUS 								\
		ZBX_POST_VSPHERE_HEADER									\
		"<ns0:RetrievePropertiesEx>"								\
			"<ns0:_this type=\"PropertyCollector\">propertyCollector</ns0:_this>"		\
			"<ns0:specSet>"									\
				"<ns0:propSet>"								\
					"<ns0:type>ClusterComputeResource</ns0:type>"			\
					"<ns0:all>false</ns0:all>"					\
					"<ns0:pathSet>summary</ns0:pathSet>"				\
				"</ns0:propSet>"							\
				"<ns0:objectSet>"							\
					"<ns0:obj type=\"ClusterComputeResource\">%s</ns0:obj>"		\
				"</ns0:objectSet>"							\
			"</ns0:specSet>"								\
			"<ns0:options></ns0:options>"							\
		"</ns0:RetrievePropertiesEx>"								\
		ZBX_POST_VSPHERE_FOOTER

	const char	*__function_name = "vmware_service_get_cluster_status";

	char		tmp[MAX_STRING_LEN];
	int		err, o, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() clusterid:'%s'", __function_name, clusterid);

	zbx_snprintf(tmp, sizeof(tmp), ZBX_POST_VMWARE_CLUSTER_STATUS, clusterid);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, o = CURLOPT_POSTFIELDS, tmp)))
	{
		*error = zbx_dsprintf(*error, "Cannot set cURL option %d: %s.", o, curl_easy_strerror(err));
		goto out;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
	{
		*error = zbx_strdup(*error, curl_easy_strerror(err));
		goto out;
	}

	zabbix_log(LOG_LEVEL_TRACE, "%s() SOAP response: %s", __function_name, page.data);

	if (NULL != (*error = zbx_xml_read_value(page.data, ZBX_XPATH_FAULTSTRING())))
		goto out;

	*status = zbx_strdup(NULL, page.data);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_get_cluster_list                                  *
 *                                                                            *
 * Purpose: creates list of vmware cluster objects                            *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             clusters     - [OUT] a pointer to the resulting cluster vector *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_get_cluster_list(const zbx_vmware_service_t *service, CURL *easyhandle,
		zbx_vector_ptr_t *clusters, char **error)
{
	const char		*__function_name = "vmware_service_get_cluster_list";

	char			*cluster_data = NULL, xpath[MAX_STRING_LEN], *name;
	zbx_vector_str_t	ids;
	zbx_vmware_cluster_t	*cluster;
	int			i, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_str_create(&ids);

	if (SUCCEED != vmware_service_get_clusters(service, easyhandle, &cluster_data, error))
		goto out;

	zbx_xml_read_values(cluster_data, "//*[@type='ClusterComputeResource']", &ids);

	for (i = 0; i < ids.values_num; i++)
	{
		char	*status;

		zbx_snprintf(xpath, sizeof(xpath), "//*[@type='ClusterComputeResource'][.='%s']"
				"/.." ZBX_XPATH_LN2("propSet", "val"), ids.values[i]);

		if (NULL == (name = zbx_xml_read_value(cluster_data, xpath)))
			continue;

		if (SUCCEED != vmware_service_get_cluster_status(service, easyhandle, ids.values[i], &status, error))
		{
			zbx_free(name);
			goto out;
		}

		cluster = zbx_malloc(NULL, sizeof(zbx_vmware_cluster_t));
		cluster->id = zbx_strdup(NULL, ids.values[i]);
		cluster->name = name;
		cluster->status = status;

		zbx_vector_ptr_append(clusters, cluster);
	}

	ret = SUCCEED;
out:
	zbx_free(cluster_data);
	zbx_vector_str_clear_ext(&ids, zbx_ptr_free);
	zbx_vector_str_destroy(&ids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s found:%d", __function_name, zbx_result_string(ret),
			clusters->values_num);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_counters_add_new                                          *
 *                                                                            *
 * Purpose: creates a new performance counter object in shared memory and     *
 *          adds to the specified vector                                      *
 *                                                                            *
 * Parameters: counters  - [IN/OUT] the vector the created performance        *
 *                                  counter object should be added to         *
 *             counterid - [IN] the performance counter id                    *
 *                                                                            *
 ******************************************************************************/
static void	vmware_counters_add_new(zbx_vector_ptr_t *counters, zbx_uint64_t counterid)
{
	zbx_vmware_perf_counter_t	*counter;

	counter = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_perf_counter_t));
	counter->counterid = counterid;
	counter->state = ZBX_VMWARE_COUNTER_NEW;

	zbx_vector_ptr_pair_create_ext(&counter->values, __vm_mem_malloc_func, __vm_mem_realloc_func,
			__vm_mem_free_func);

	zbx_vector_ptr_append(counters, counter);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_initialize                                        *
 *                                                                            *
 * Purpose: initializes vmware service object                                 *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *             easyhandle   - [IN] the CURL handle                            *
 *             error        - [OUT] the error message in the case of failure  *
 *                                                                            *
 * Return value: SUCCEED - the operation has completed successfully           *
 *               FAIL    - the operation has failed                           *
 *                                                                            *
 * Comments: While the service object can't be accessed from other processes  *
 *           during initialization it's still processed outside vmware locks  *
 *           and therefore must not allocate/free shared memory.              *
 *                                                                            *
 ******************************************************************************/
static int	vmware_service_initialize(zbx_vmware_service_t *service, CURL *easyhandle, char **error)
{
	char			*contents = NULL;
	zbx_vector_ptr_t	counters;
	int			ret = FAIL;

	zbx_vector_ptr_create(&counters);

	if (SUCCEED != vmware_service_get_perf_counters(service, easyhandle, &counters, error))
		goto out;

	if (SUCCEED != vmware_service_get_contents(service, easyhandle, &contents, error))
		goto out;

	zbx_vmware_lock();

	service->contents = vmware_shared_strdup(contents);
	vmware_counters_shared_copy(&service->counters, &counters);

	zbx_vmware_unlock();

	ret = SUCCEED;
out:
	zbx_free(contents);

	zbx_vector_ptr_clear_ext(&counters, (zbx_mem_free_func_t)vmware_counter_free);
	zbx_vector_ptr_destroy(&counters);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_add_perf_entity                                   *
 *                                                                            *
 * Purpose: adds entity to vmware service performance entity list             *
 *                                                                            *
 * Parameters: service  - [IN] the vmware service                             *
 *             type     - [IN] the performance entity type (HostSystem,       *
 *                             (VirtualMachine...)                            *
 *             id       - [IN] the performance entity id                      *
 *             counters - [IN] NULL terminated list of performance counters   *
 *                             to be monitored for this entity                *
 *             now      - [IN] the current timestamp                          *
 *                                                                            *
 * Comments: The performance counters are specified by their path:            *
 *             <group>/<key>[<rollup type>]                                   *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_add_perf_entity(zbx_vmware_service_t *service, const char *type, const char *id,
		const char **counters, int now)
{
	const char			*__function_name = "vmware_service_add_perf_entity";

	zbx_vmware_perf_entity_t	entity, *pentity;
	zbx_uint64_t			counterid;
	int				i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() type:%s id:%s", __function_name, type, id);

	if (NULL == (pentity = zbx_vmware_service_get_perf_entity(service, type, id)))
	{
		entity.type = vmware_shared_strdup(type);
		entity.id = vmware_shared_strdup(id);

		pentity = zbx_hashset_insert(&service->entities, &entity, sizeof(zbx_vmware_perf_entity_t));

		zbx_vector_ptr_create_ext(&pentity->counters, __vm_mem_malloc_func, __vm_mem_realloc_func,
				__vm_mem_free_func);

		for (i = 0; NULL != counters[i]; i++)
		{
			if (SUCCEED == zbx_vmware_service_get_counterid(service, counters[i], &counterid))
				vmware_counters_add_new(&pentity->counters, counterid);
			else
				zabbix_log(LOG_LEVEL_DEBUG, "cannot find performance counter %s", counters[i]);
		}

		zbx_vector_ptr_sort(&pentity->counters, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);
		pentity->refresh = 0;
	}

	pentity->last_seen = now;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() perfcounters:%d", __function_name, pentity->counters.values_num);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_update_perf_entities                              *
 *                                                                            *
 * Purpose: adds new or remove old entities (hypervisors, virtual machines)   *
 *          from service performance entity list                              *
 *                                                                            *
 * Parameters: service - [IN] the vmware service                              *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_update_perf_entities(zbx_vmware_service_t *service)
{
	const char			*__function_name = "vmware_service_update_perf_entities";

	int				now, i, j;
	zbx_vmware_perf_entity_t	*entity;
	zbx_vmware_hv_t			*hv;
	zbx_vmware_vm_t			*vm;
	zbx_hashset_iter_t		iter;

	const char			*hv_perfcounters[] = {
						"net/packetsRx[summation]", "net/packetsTx[summation]",
						"net/received[average]", "net/transmitted[average]",
						"datastore/totalReadLatency[average]",
						"datastore/totalWriteLatency[average]", NULL
					};
	const char			*vm_perfcounters[] = {
						"virtualDisk/read[average]", "virtualDisk/write[average]",
						"virtualDisk/numberReadAveraged[average]",
						"virtualDisk/numberWriteAveraged[average]",
						"net/packetsRx[summation]", "net/packetsTx[summation]",
						"net/received[average]", "net/transmitted[average]",
						"cpu/ready[summation]", NULL
					};

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	now = time(NULL);

	/* update current performance entities */
	for (i = 0; i < service->data->hvs.values_num; i++)
	{
		hv = service->data->hvs.values[i];
		vmware_service_add_perf_entity(service, "HostSystem", hv->id, hv_perfcounters, now);

		for (j = 0; j < hv->vms.values_num; j++)
		{
			vm = hv->vms.values[j];
			vmware_service_add_perf_entity(service, "VirtualMachine", vm->id, vm_perfcounters, now);
		}
	}

	/* remove old entities */
	zbx_hashset_iter_reset(&service->entities, &iter);
	while (NULL != (entity = zbx_hashset_iter_next(&iter)))
	{
		if (0 != entity->last_seen && entity->last_seen < now)
		{
			vmware_shared_perf_entity_clean(entity);
			zbx_hashset_iter_remove(&iter);
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() entities:%d", __function_name, service->entities.num_data);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_update                                            *
 *                                                                            *
 * Purpose: updates object with a new data from vmware service                *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_update(zbx_vmware_service_t *service)
{
	const char		*__function_name = "vmware_service_update";

	CURL			*easyhandle = NULL;
	struct curl_slist	*headers = NULL;
	zbx_vmware_data_t	*data;
	zbx_vector_str_t	hvs;
	int			opt, err, i, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() %s@%s", __function_name, service->username, service->url);

	data = zbx_malloc(NULL, sizeof(zbx_vmware_data_t));
	memset(data, 0, sizeof(zbx_vmware_data_t));

	zbx_vector_ptr_create(&data->hvs);
	zbx_vector_ptr_create(&data->clusters);

	zbx_vector_str_create(&hvs);

	if (NULL == (easyhandle = curl_easy_init()))
	{
		zabbix_log(LOG_LEVEL_WARNING, "Cannot initialize cURL library");
		goto out;
	}

	headers = curl_slist_append(headers, ZBX_XML_HEADER1);
	headers = curl_slist_append(headers, ZBX_XML_HEADER2);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_HTTPHEADER, headers)))
	{
		zabbix_log(LOG_LEVEL_WARNING, "Cannot set cURL option %d: %s", opt, curl_easy_strerror(err));
		goto clean;
	}

	if (SUCCEED != vmware_service_authenticate(service, easyhandle, &data->error))
		goto clean;

	if (0 != (service->state & ZBX_VMWARE_STATE_NEW) &&
			SUCCEED != vmware_service_initialize(service, easyhandle, &data->error))
	{
		goto clean;
	}

	if (SUCCEED != vmware_service_get_hv_list(service, easyhandle, &hvs, &data->error))
		goto clean;

	for (i = 0; i < hvs.values_num; i++)
	{
		zbx_vmware_hv_t	*hv;

		if (NULL != (hv = vmware_service_create_hv(service, easyhandle, hvs.values[i], &data->error)))
			zbx_vector_ptr_append(&data->hvs, hv);
	}

	if (SUCCEED != vmware_service_get_event_data(service, easyhandle, &data->events, &data->error))
		goto clean;

	if (ZBX_VMWARE_TYPE_VCENTER == service->type &&
			SUCCEED != vmware_service_get_cluster_list(service, easyhandle, &data->clusters, &data->error))
	{
		goto clean;
	}

	ret = SUCCEED;
clean:
	curl_slist_free_all(headers);
	curl_easy_cleanup(easyhandle);

	zbx_vector_str_clear_ext(&hvs, zbx_ptr_free);
	zbx_vector_str_destroy(&hvs);
out:
	zbx_vmware_lock();

	/* remove UPDATING flag and set READY or FAILED flag */
	service->state &= ~(ZBX_VMWARE_STATE_MASK | ZBX_VMWARE_STATE_UPDATING);
	service->state |= (SUCCEED == ret) ? ZBX_VMWARE_STATE_READY : ZBX_VMWARE_STATE_FAILED;

	vmware_data_shared_free(service->data);
	service->data = vmware_data_shared_dup(data);
	vmware_data_free(data);

	service->lastcheck = time(NULL);

	vmware_service_update_perf_entities(service);

	zbx_vmware_unlock();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_process_perf_entity_data                          *
 *                                                                            *
 * Purpose: updates vmware performance statistics data                        *
 *                                                                            *
 * Parameters: service - [IN] the vmware service                              *
 *             type    - [IN] the performance entity type (HostSystem,        *
 *                            (VirtualMachine...)                             *
 *             id      - [IN] the performance entity id                       *
 *             doc     - [IN] the XML document containing performance counter *
 *                            values for all entities                         *
 *             node    - [IN] the XML node containing performance counter     *
 *                            values for the specified entity                 *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_process_perf_entity_data(zbx_vmware_service_t *service, const char *type, const char *id,
		xmlDoc *doc, xmlNode *node)
{
	const char			*__function_name = "vmware_service_process_perf_entity_data";

	xmlXPathContext			*xpathCtx;
	xmlXPathObject			*xpathObj;
	xmlNodeSetPtr			nodeset;
	char				*instance, *counter, *value;
	int				i, index, values = 0, valid_data = 0;
	zbx_vmware_perf_entity_t	*entity;
	zbx_uint64_t			counterid;
	zbx_vmware_perf_counter_t	*perfcounter;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() type:%s id:%s", __function_name, type, id);

	xpathCtx = xmlXPathNewContext(doc);
	xpathCtx->node = node;

	if (NULL == (xpathObj = xmlXPathEvalExpression((xmlChar *)"*[local-name()='value']", xpathCtx)))
		goto out;

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto out;

	if (NULL == (entity = zbx_vmware_service_get_perf_entity(service, type, id)))
		goto out;

	nodeset = xpathObj->nodesetval;

	for (i = 0; i < nodeset->nodeNr; i++)
	{
		value = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='value']");
		instance = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='id']"
				"/*[local-name()='instance']");
		counter = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='id']"
				"/*[local-name()='counterId']");

		if (NULL != value && NULL != counter)
		{
			ZBX_STR2UINT64(counterid, counter);

			if (FAIL != (index = zbx_vector_ptr_bsearch(&entity->counters, &counterid,
					ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
			{
				zbx_ptr_pair_t	perfvalue;
				perfcounter = (zbx_vmware_perf_counter_t *)entity->counters.values[index];

				perfvalue.first = vmware_shared_strdup(NULL != instance ? instance : "");
				perfvalue.second = vmware_shared_strdup(value);

				zbx_vector_ptr_pair_append_ptr(&perfcounter->values, &perfvalue);
				values++;

				if (0 == valid_data && 0 != strcmp(value, "-1"))
					valid_data = 1;
			}
		}

		zbx_free(counter);
		zbx_free(instance);
		zbx_free(value);
	}

	/* No valid data found - all metrics have -1 value. In this case clear the counter values. */
	if (0 == valid_data)
	{
		for (i = 0; i < entity->counters.values_num; i++)
		{
			perfcounter = (zbx_vmware_perf_counter_t *)entity->counters.values[i];
			vmware_vector_ptr_pair_shared_clean(&perfcounter->values);
		}
	}
out:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() values:%d", __function_name, values);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_parse_perf_data                                   *
 *                                                                            *
 * Purpose: updates vmware performance statistics data                        *
 *                                                                            *
 * Parameters: service - [IN] the vmware service                              *
 *             data    - [IN] the performance data                            *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_parse_perf_data(zbx_vmware_service_t *service, const char *data)
{
	const char		*__function_name = "vmware_service_parse_perf_data";

	xmlDoc			*doc;
	xmlXPathContext		*xpathCtx;
	xmlXPathObject		*xpathObj;
	xmlNodeSetPtr		nodeset;
	int			i;
	char			*id, *type;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL == (doc = xmlReadMemory(data, strlen(data), ZBX_VM_NONAME_XML, NULL, 0)))
		goto out;

	xpathCtx = xmlXPathNewContext(doc);

	if (NULL == (xpathObj = xmlXPathEvalExpression((xmlChar *)"/*/*/*/*", xpathCtx)))
		goto clean;

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto clean;

	nodeset = xpathObj->nodesetval;

	for (i = 0; i < nodeset->nodeNr; i++)
	{
		id = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='entity']");
		type = zbx_xml_read_node_value(doc, nodeset->nodeTab[i], "*[local-name()='entity']/@type");

		if (NULL != type && NULL != id)
			vmware_service_process_perf_entity_data(service, type, id, doc, nodeset->nodeTab[i]);

		zbx_free(type);
		zbx_free(id);
	}
clean:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_update_perf                                       *
 *                                                                            *
 * Purpose: updates vmware statistics data                                    *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_update_perf(zbx_vmware_service_t *service)
{
	const char			*__function_name = "vmware_service_update_perf";

	CURL				*easyhandle = NULL;
	struct curl_slist		*headers = NULL;
	int				err, opt, i, j;
	char				*error = NULL, *tmp = NULL;
	size_t				tmp_alloc = 0, tmp_offset = 0;
	zbx_vector_ptr_t		entities;
	zbx_vmware_perf_entity_t	*entity, *local_entity;
	zbx_vmware_perf_counter_t	*counter;
	zbx_hashset_iter_t		iter;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() %s@%s", __function_name, service->username, service->url);

	if (NULL == (easyhandle = curl_easy_init()))
	{
		error = zbx_strdup(error, "cannot initialize cURL library");
		goto out;
	}

	headers = curl_slist_append(headers, ZBX_XML_HEADER1);
	headers = curl_slist_append(headers, ZBX_XML_HEADER2);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_HTTPHEADER, headers)))
	{
		error = zbx_dsprintf(error, "cannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto clean;
	}

	if (SUCCEED != vmware_service_authenticate(service, easyhandle, &error))
		goto clean;

	/* update performance counter refresh rate for entities */

	/* create a local list of entities with zero refresh rate */
	zbx_vector_ptr_create(&entities);

	zbx_vmware_lock();

	zbx_hashset_iter_reset(&service->entities, &iter);
	while (NULL != (entity = zbx_hashset_iter_next(&iter)))
	{
		if (0 != entity->refresh)
			continue;

		local_entity = zbx_malloc(NULL, sizeof(zbx_vmware_perf_entity_t));
		local_entity->type = zbx_strdup(NULL, entity->type);
		local_entity->id = zbx_strdup(NULL, entity->id);
		local_entity->refresh = 0;

		zbx_vector_ptr_append(&entities, local_entity);
	}

	zbx_vmware_unlock();

	/* get refresh rates */
	for (i = 0; i < entities.values_num; i++)
	{
		local_entity = entities.values[i];

		if (SUCCEED != vmware_service_get_perf_counter_refreshrate(service, easyhandle, local_entity->type,
				local_entity->id, &local_entity->refresh, &error))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot get refresh rate for %s \"%s\": %s", local_entity->type,
					local_entity->id, error);
			zbx_free(error);
		}
	}

	/* update refresh rates and create performance query request */
	zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, ZBX_POST_VSPHERE_HEADER);
	zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, "<ns0:QueryPerf>");
	zbx_snprintf_alloc(&tmp, &tmp_alloc, &tmp_offset, "<ns0:_this type=\"PerformanceManager\">%s</ns0:_this>",
			vmware_service_objects[service->type].performance_manager);

	zbx_vmware_lock();

	/* udpate entity refresh rate */
	for (i = 0; i < entities.values_num; i++)
	{
		if (NULL != (entity = zbx_hashset_search(&service->entities, entities.values[i])))
			entity->refresh = ((zbx_vmware_perf_entity_t *)entities.values[i])->refresh;
	}

	/* create performance collector request */
	zbx_hashset_iter_reset(&service->entities, &iter);
	while (NULL != (entity = zbx_hashset_iter_next(&iter)))
	{
		if (0 == entity->refresh)
		{
			zabbix_log(LOG_LEVEL_DEBUG, "skipping performance entity with zero refresh rate "
					"type:%s id:%d", entity->type, entity->id);
			continue;
		}

		/* add entity performance counter request */
		zbx_snprintf_alloc(&tmp, &tmp_alloc, &tmp_offset, "<ns0:querySpec>"
				"<ns0:entity type=\"%s\">%s</ns0:entity><ns0:maxSample>1</ns0:maxSample>",
				entity->type, entity->id);

		for (j = 0; j < entity->counters.values_num; j++)
		{
			counter = (zbx_vmware_perf_counter_t *)entity->counters.values[j];

			zbx_snprintf_alloc(&tmp, &tmp_alloc, &tmp_offset, "<ns0:metricId><ns0:counterId>" ZBX_FS_UI64
					"</ns0:counterId><ns0:instance>*</ns0:instance></ns0:metricId>",
					counter->counterid);

			counter->state |= ZBX_VMWARE_COUNTER_UPDATING;
		}

		zbx_snprintf_alloc(&tmp, &tmp_alloc, &tmp_offset, "<ns0:intervalId>%d</ns0:intervalId></ns0:querySpec>",
				entity->refresh);
	}

	zbx_vmware_unlock();

	zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, "</ns0:QueryPerf>");
	zbx_strcpy_alloc(&tmp, &tmp_alloc, &tmp_offset, ZBX_POST_VSPHERE_FOOTER);

	zbx_vector_ptr_clear_ext(&entities, (zbx_mem_free_func_t)vmware_perf_entity_free);
	zbx_vector_ptr_destroy(&entities);

	if (CURLE_OK != (err = curl_easy_setopt(easyhandle, opt = CURLOPT_POSTFIELDS, tmp)))
	{
		error = zbx_dsprintf(error, "dannot set cURL option %d: %s.", opt, curl_easy_strerror(err));
		goto clean;
	}

	page.offset = 0;

	if (CURLE_OK != (err = curl_easy_perform(easyhandle)))
		error = zbx_strdup(error, curl_easy_strerror(err));
clean:
	zbx_free(tmp);

	curl_slist_free_all(headers);
	curl_easy_cleanup(easyhandle);
out:
	zbx_vmware_lock();

	if (NULL != error)
	{
		zabbix_log(LOG_LEVEL_WARNING, "cannot update performance statistics of vmware service \"%s\": %s",
				service->url, error);
		zbx_free(error);
	}
	else
	{
		vmware_entities_shared_clean_stats(&service->entities);
		vmware_service_parse_perf_data(service, page.data);
	}

	service->state &= ~(ZBX_VMWARE_STATE_UPDATING_PERF);
	service->lastperfcheck = time(NULL);

	zbx_vmware_unlock();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): processed " ZBX_FS_SIZE_T " bytes of data", __function_name,
			(zbx_fs_size_t)page.offset);
}

/******************************************************************************
 *                                                                            *
 * Function: vmware_service_remove                                            *
 *                                                                            *
 * Purpose: removes vmware service                                            *
 *                                                                            *
 * Parameters: service      - [IN] the vmware service                         *
 *                                                                            *
 ******************************************************************************/
static void	vmware_service_remove(zbx_vmware_service_t *service)
{
	const char	*__function_name = "vmware_service_remove";
	int		index;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() %s@%s", __function_name, service->username, service->url);

	zbx_vmware_lock();

	if (FAIL != (index = zbx_vector_ptr_search(&vmware->services, &service, ZBX_DEFAULT_PTR_COMPARE_FUNC)))
	{
		zbx_vector_ptr_remove(&vmware->services, index);
		vmware_service_shared_free(service);
	}

	zbx_vmware_unlock();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()");
}

/*
 * Public API
 */

/******************************************************************************
 *                                                                            *
 * Function: vmware_get_service                                               *
 *                                                                            *
 * Purpose: gets vmware service object                                        *
 *                                                                            *
 * Parameters: url      - [IN] the vmware service URL                         *
 *             username - [IN] the vmware service username                    *
 *             password - [IN] the vmware service password                    *
 *                                                                            *
 * Return value: the requested service object or NULL if the object is not    *
 *               yet ready.                                                   *
 *                                                                            *
 * Comments: vmware lock must be locked with zbx_vmware_lock() function       *
 *           before calling this function.                                    *
 *           If the service list does not contain the requested service object*
 *           then a new object is created, marked as new, added to the list   *
 *           and a NULL value is returned.                                    *
 *           If the object is in list, but is not yet updated also a NULL     *
 *           value is returned.                                               *
 *                                                                            *
 ******************************************************************************/
zbx_vmware_service_t	*zbx_vmware_get_service(const char* url, const char* username, const char* password)
{
	const char		*__function_name = "zbx_vmware_get_service";

	int			i, now;
	zbx_vmware_service_t	*service = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() %s@%s", __function_name, username, url);

	if (NULL == vmware)
		goto out;

	now = time(NULL);

	for (i = 0; i < vmware->services.values_num; i++)
	{
		service = vmware->services.values[i];

		if (0 == strcmp(service->url, url) && 0 == strcmp(service->username, username) &&
				0 == strcmp(service->password, password))
		{
			service->lastaccess = now;

			/* return NULL if the service is not ready yet */
			if (0 == (service->state & (ZBX_VMWARE_STATE_READY | ZBX_VMWARE_STATE_FAILED)))
				service = NULL;

			goto out;
		}
	}

	service = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_service_t));
	memset(service, 0, sizeof(zbx_vmware_service_t));

	service->url = vmware_shared_strdup(url);
	service->username = vmware_shared_strdup(username);
	service->password = vmware_shared_strdup(password);
	service->type = ZBX_VMWARE_TYPE_UNKNOWN;
	service->state = ZBX_VMWARE_STATE_NEW;
	service->lastaccess = now;

	zbx_hashset_create_ext(&service->entities, 100, vmware_perf_entity_hash_func,  vmware_perf_entity_compare_func,
			NULL, __vm_mem_malloc_func, __vm_mem_realloc_func, __vm_mem_free_func);

	zbx_hashset_create_ext(&service->counters, ZBX_VMWARE_COUNTERS_INIT_SIZE, vmware_counter_hash_func,
			vmware_counter_compare_func, NULL, __vm_mem_malloc_func, __vm_mem_realloc_func,
			__vm_mem_free_func);

	zbx_vector_ptr_append(&vmware->services, service);

	/* new service does not have any data - return NULL */
	service = NULL;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name,
			zbx_result_string(NULL != service ? SUCCEED : FAIL));

	return service;
}


/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_service_get_counterid                                 *
 *                                                                            *
 * Purpose: gets vmware performance counter id by the path                    *
 *                                                                            *
 * Parameters: service   - [IN] the vmware service                            *
 *             path      - [IN] the path of counter to retrieve in format     *
 *                              <group>/<key>[<rollup type>]                  *
 *             counterid - [OUT] the counter id                               *
 *                                                                            *
 * Return value: SUCCEED if the counter was found, FAIL otherwise             *
 *                                                                            *
 ******************************************************************************/
int	zbx_vmware_service_get_counterid(zbx_vmware_service_t *service, const char *path,
		zbx_uint64_t *counterid)
{
#if defined(HAVE_LIBXML2) && defined(HAVE_LIBCURL)
	const char		*__function_name = "zbx_vmware_service_get_perfcounterid";
	zbx_vmware_counter_t	*counter;
	int			ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() path:%s", __function_name, path);

	if (NULL == (counter = zbx_hashset_search(&service->counters, &path)))
		goto out;

	*counterid = counter->id;

	zabbix_log(LOG_LEVEL_DEBUG, "%s() counterid:" ZBX_FS_UI64, __function_name, *counterid);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
#else
	return FAIL;
#endif
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_service_add_perf_counter                              *
 *                                                                            *
 * Purpose: start monitoring performance counter of the specified entity      *
 *                                                                            *
 * Parameters: service   - [IN] the vmware service                            *
 *             type      - [IN] the entity type                               *
 *             id        - [IN] the entity id                                 *
 *             counterid - [IN] the performance counter id                    *
 *                                                                            *
 * Return value: SUCCEED - the entity counter was added to monitoring list.   *
 *               FAIL    - the performance counter of the specified entity    *
 *                         is already being monitored.                        *
 *                                                                            *
 ******************************************************************************/
int	zbx_vmware_service_add_perf_counter(zbx_vmware_service_t *service, const char *type, const char *id,
		zbx_uint64_t counterid)
{
	const char			*__function_name = "zbx_vmware_service_start_monitoring";
	zbx_vmware_perf_entity_t	*pentity, entity;
	int				ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() type:%s id:%s counterid:" ZBX_FS_UI64, __function_name, type, id,
			counterid);

	if (NULL == (pentity = zbx_vmware_service_get_perf_entity(service, type, id)))
	{
		entity.refresh = 0;
		entity.last_seen = 0;
		entity.type = vmware_shared_strdup(type);
		entity.id = vmware_shared_strdup(id);
		zbx_vector_ptr_create_ext(&entity.counters, __vm_mem_malloc_func, __vm_mem_realloc_func,
				__vm_mem_free_func);

		pentity = zbx_hashset_insert(&service->entities, &entity, sizeof(zbx_vmware_perf_entity_t));
	}

	if (FAIL == zbx_vector_ptr_search(&pentity->counters, &counterid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC))
	{
		vmware_counters_add_new(&pentity->counters, counterid);
		zbx_vector_ptr_sort(&pentity->counters, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

		ret = SUCCEED;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_service_get_perf_entity                               *
 *                                                                            *
 * Purpose: gets performance entity by type and id                            *
 *                                                                            *
 * Parameters: service - [IN] the vmware service                              *
 *             type    - [IN] the performance entity type                     *
 *             id      - [IN] the performance entity id                       *
 *                                                                            *
 * Return value: the performance entity or NULL if not found                  *
 *                                                                            *
 ******************************************************************************/
zbx_vmware_perf_entity_t	*zbx_vmware_service_get_perf_entity(zbx_vmware_service_t *service, const char *type,
		const char *id)
{
	const char			*__function_name = "zbx_vmware_service_get_perf_entity";

	zbx_vmware_perf_entity_t	*pentity, entity = {(char *)type, (char *)id};

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() type:%s id:%s", __function_name, type, id);

	pentity = zbx_hashset_search(&service->entities, &entity);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() entity:%p", __function_name, pentity);

	return pentity;
}
#endif

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_init                                                  *
 *                                                                            *
 * Purpose: initializes vmware collector service                              *
 *                                                                            *
 * Comments: This function must be called before worker threads are forked.   *
 *                                                                            *
 ******************************************************************************/
void	zbx_vmware_init(void)
{
	const char	*__function_name = "zbx_vmware_init";

	key_t		shm_key;
	zbx_uint64_t	size_reserved;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_mutex_create(&vmware_lock, ZBX_MUTEX_VMWARE);

	if (-1 == (shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_VMWARE_ID)))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot create IPC key for vmware cache");
		exit(EXIT_FAILURE);
	}

	size_reserved = zbx_mem_required_size(1, "vmware cache size", "VMwareCacheSize");

	CONFIG_VMWARE_CACHE_SIZE -= size_reserved;

	zbx_mem_create(&vmware_mem, shm_key, ZBX_NO_MUTEX, CONFIG_VMWARE_CACHE_SIZE, "vmware cache size",
			"VMwareCacheSize", 0);

	vmware = __vm_mem_malloc_func(NULL, sizeof(zbx_vmware_t));
	memset(vmware, 0, sizeof(zbx_vmware_t));

	VMWARE_VECTOR_CREATE(&vmware->services, ptr);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_destroy                                               *
 *                                                                            *
 * Purpose: destroys vmware collector service                                 *
 *                                                                            *
 ******************************************************************************/
void	zbx_vmware_destroy(void)
{
	const char	*__function_name = "zbx_vmware_destroy";

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_mem_destroy(vmware_mem);
	zbx_mutex_destroy(&vmware_lock);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

#define	ZBX_VMWARE_TASK_NONE		0
#define	ZBX_VMWARE_TASK_IDLE		1
#define	ZBX_VMWARE_TASK_UPDATE		2
#define	ZBX_VMWARE_TASK_UPDATE_PERF	3
#define	ZBX_VMWARE_TASK_REMOVE		4

/******************************************************************************
 *                                                                            *
 * Function: main_vmware_loop                                                 *
 *                                                                            *
 * Purpose: the vmware collector main loop                                    *
 *                                                                            *
 ******************************************************************************/
ZBX_THREAD_ENTRY(vmware_thread, args)
{
#if defined(HAVE_LIBXML2) && defined(HAVE_LIBCURL)
	int			i, now, task, next_update, updated_services = 0, removed_services = 0,
				old_updated_services = 0, old_removed_services = 0, sleeptime = -1;
	zbx_vmware_service_t	*service = NULL;
	double			sec, total_sec = 0.0, old_total_sec = 0.0;
	time_t			last_stat_time;

	process_type = ((zbx_thread_args_t *)args)->process_type;
	server_num = ((zbx_thread_args_t *)args)->server_num;
	process_num = ((zbx_thread_args_t *)args)->process_num;

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(program_type),
			server_num, get_process_type_string(process_type), process_num);

#define STAT_INTERVAL	5	/* if a process is busy and does not sleep then update status not faster than */
				/* once in STAT_INTERVAL seconds */

	last_stat_time = time(NULL);

	for (;;)
	{
		zbx_handle_log();

		if (0 != sleeptime)
		{
			zbx_setproctitle("%s #%d [updated %d, removed %d VMware services in " ZBX_FS_DBL " sec, "
					"querying VMware services]", get_process_type_string(process_type), process_num,
					old_updated_services, old_removed_services, old_total_sec);
		}

		sec = zbx_time();

		do
		{
			task = ZBX_VMWARE_TASK_IDLE;

			now = time(NULL);
			next_update = now + POLLER_DELAY;

			zbx_vmware_lock();

			/* find a task to be performed on a vmware service */
			for (i = 0; i < vmware->services.values_num; i++)
			{
				service = vmware->services.values[i];

				/* check if the service isn't used and should be removed */
				if (0 == (service->state & ZBX_VMWARE_STATE_BUSY) &&
						now - service->lastaccess > ZBX_VMWARE_SERVICE_TTL)
				{
					service->state |= ZBX_VMWARE_STATE_REMOVING;
					task = ZBX_VMWARE_TASK_REMOVE;
					break;
				}

				/* check if the performance statistics should be updated */
				if (0 != (service->state & ZBX_VMWARE_STATE_READY) &&
						0 == (service->state & ZBX_VMWARE_STATE_UPDATING_PERF) &&
						now - service->lastperfcheck >= ZBX_VMWARE_PERF_UPDATE_PERIOD)
				{
					service->state |= ZBX_VMWARE_STATE_UPDATING_PERF;
					task = ZBX_VMWARE_TASK_UPDATE_PERF;
					break;
				}

				/* check if the service data should be updated */
				if (0 == (service->state & ZBX_VMWARE_STATE_UPDATING) &&
						now - service->lastcheck >= ZBX_VMWARE_CACHE_UPDATE_PERIOD)
				{
					service->state |= ZBX_VMWARE_STATE_UPDATING;
					task = ZBX_VMWARE_TASK_UPDATE;
					break;
				}

				/* don't calculate nextcheck for services that are already updating something */
				if (0 != (service->state & ZBX_VMWARE_STATE_BUSY))
						continue;

				/* calculate next service update time */

				if (service->lastcheck + ZBX_VMWARE_CACHE_UPDATE_PERIOD < next_update)
					next_update = service->lastcheck + ZBX_VMWARE_CACHE_UPDATE_PERIOD;

				if (0 != (service->state & ZBX_VMWARE_STATE_READY))
				{
					if (service->lastperfcheck + ZBX_VMWARE_PERF_UPDATE_PERIOD < next_update)
						next_update = service->lastperfcheck + ZBX_VMWARE_PERF_UPDATE_PERIOD;
				}
			}

			zbx_vmware_unlock();

			switch (task)
			{
				case ZBX_VMWARE_TASK_UPDATE:
					vmware_service_update(service);
					updated_services++;
					break;
				case ZBX_VMWARE_TASK_UPDATE_PERF:
					vmware_service_update_perf(service);
					updated_services++;
					break;
				case ZBX_VMWARE_TASK_REMOVE:
					vmware_service_remove(service);
					removed_services++;
					break;
			}
		}
		while (ZBX_VMWARE_TASK_IDLE != task);

		total_sec += zbx_time() - sec;
		now = time(NULL);

		sleeptime = 0 < next_update - now ? next_update - now : 0;

		if (0 != sleeptime || STAT_INTERVAL <= time(NULL) - last_stat_time)
		{
			if (0 == sleeptime)
			{
				zbx_setproctitle("%s #%d [updated %d, removed %d VMware services in " ZBX_FS_DBL " sec,"
						" querying VMware services]", get_process_type_string(process_type),
						process_num, updated_services, removed_services, total_sec);
			}
			else
			{
				zbx_setproctitle("%s #%d [updated %d, removed %d VMware services in " ZBX_FS_DBL " sec,"
						" idle %d sec]", get_process_type_string(process_type), process_num,
						updated_services, removed_services, total_sec, sleeptime);
				old_updated_services = updated_services;
				old_removed_services = removed_services;
				old_total_sec = total_sec;
			}
			updated_services = 0;
			removed_services = 0;
			total_sec = 0.0;
			last_stat_time = time(NULL);
		}

		zbx_sleep_loop(sleeptime);
	}
#undef STAT_INTERVAL
#else
	zbx_thread_exit(EXIT_SUCCESS);
#endif
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_lock                                                  *
 *                                                                            *
 * Purpose: locks vmware collector                                            *
 *                                                                            *
 ******************************************************************************/
void	zbx_vmware_lock(void)
{
	zbx_mutex_lock(&vmware_lock);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_unlock                                                *
 *                                                                            *
 * Purpose: unlocks vmware collector                                          *
 *                                                                            *
 ******************************************************************************/
void	zbx_vmware_unlock(void)
{
	zbx_mutex_unlock(&vmware_lock);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_vmware_get_statistics                                        *
 *                                                                            *
 * Purpose: gets vmware collector statistics                                  *
 *                                                                            *
 * Parameters: stats   - [OUT] the vmware collector statistics                *
 *                                                                            *
 * Return value: SUCCEEED - the statistics were retrieved successfully        *
 *               FAIL     - no vmware collectors are running                  *
 *                                                                            *
 ******************************************************************************/
int	zbx_vmware_get_statistics(zbx_vmware_stats_t *stats)
{
	if (NULL == vmware_mem)
		return FAIL;

	zbx_vmware_lock();

	stats->memory_total = vmware_mem->total_size;
	stats->memory_used = vmware_mem->used_size;

	zbx_vmware_unlock();

	return SUCCEED;
}

#if defined(HAVE_LIBXML2) && defined(HAVE_LIBCURL)

/*
 * XML support
 */

/******************************************************************************
 *                                                                            *
 * Function: zbx_xml_read_value                                               *
 *                                                                            *
 * Purpose: retrieve a value from xml data                                    *
 *                                                                            *
 * Parameters: data   - [IN] XML data                                         *
 *             xpath  - [IN] XML XPath                                        *
 *                                                                            *
 * Return: The allocated value string or NULL if the xml data does not        *
 *         contain the value specified by xpath.                              *
 *                                                                            *
 ******************************************************************************/
char	*zbx_xml_read_value(const char *data, const char *xpath)
{
	xmlDoc		*doc;
	xmlXPathContext	*xpathCtx;
	xmlXPathObject	*xpathObj;
	xmlNodeSetPtr	nodeset;
	xmlChar		*val;
	char		*value = NULL;

	if (NULL == data)
		goto out;

	if (NULL == (doc = xmlReadMemory(data, strlen(data), ZBX_VM_NONAME_XML, NULL, 0)))
		goto out;

	xpathCtx = xmlXPathNewContext(doc);

	if (NULL == (xpathObj = xmlXPathEvalExpression((const xmlChar *)xpath, xpathCtx)))
		goto clean;

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto clean;

	nodeset = xpathObj->nodesetval;

	if (NULL != (val = xmlNodeListGetString(doc, nodeset->nodeTab[0]->xmlChildrenNode, 1)))
	{
		value = zbx_strdup(NULL, (char *)val);
		xmlFree(val);
	}
clean:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
out:
	return value;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_xml_read_node_value                                          *
 *                                                                            *
 * Purpose: retrieve a value from xml data relative to the specified node     *
 *                                                                            *
 * Parameters: doc    - [IN] the XML document                                 *
 *             node   - [IN] the XML node                                     *
 *             xpath  - [IN] the XML XPath                                    *
 *                                                                            *
 * Return: The allocated value string or NULL if the xml data does not        *
 *         contain the value specified by xpath.                              *
 *                                                                            *
 ******************************************************************************/
char	*zbx_xml_read_node_value(xmlDoc *doc, xmlNode *node, const char *xpath)
{
	xmlXPathContext	*xpathCtx;
	xmlXPathObject	*xpathObj;
	xmlNodeSetPtr	nodeset;
	xmlChar		*val;
	char		*value = NULL;

	xpathCtx = xmlXPathNewContext(doc);

	xpathCtx->node = node;

	if (NULL == (xpathObj = xmlXPathEvalExpression((const xmlChar *)xpath, xpathCtx)))
		goto clean;

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto clean;

	nodeset = xpathObj->nodesetval;

	if (NULL != (val = xmlNodeListGetString(doc, nodeset->nodeTab[0]->xmlChildrenNode, 1)))
	{
		value = zbx_strdup(NULL, (char *)val);
		xmlFree(val);
	}
clean:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);

	return value;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_xml_read_values                                              *
 *                                                                            *
 * Purpose: populate array of values from a xml data                          *
 *                                                                            *
 * Parameters: data   - [IN] XML data                                         *
 *             xpath  - [IN] XML XPath                                        *
 *             values - [OUT] list of requested values                        *
 *                                                                            *
 * Return: Upon successful completion the function return SUCCEED.            *
 *         Otherwise, FAIL is returned.                                       *
 *                                                                            *
 ******************************************************************************/
int	zbx_xml_read_values(const char *data, const char *xpath, zbx_vector_str_t *values)
{
	xmlDoc		*doc;
	xmlXPathContext	*xpathCtx;
	xmlXPathObject	*xpathObj;
	xmlNodeSetPtr	nodeset;
	xmlChar		*val;
	int		i, ret = FAIL;

	if (NULL == data)
		goto out;

	if (NULL == (doc = xmlReadMemory(data, strlen(data), ZBX_VM_NONAME_XML, NULL, 0)))
		goto out;

	xpathCtx = xmlXPathNewContext(doc);

	if (NULL == (xpathObj = xmlXPathEvalExpression((xmlChar *)xpath, xpathCtx)))
		goto clean;

	if (0 != xmlXPathNodeSetIsEmpty(xpathObj->nodesetval))
		goto clean;

	nodeset = xpathObj->nodesetval;

	for (i = 0; i < nodeset->nodeNr; i++)
	{
		if (NULL != (val = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1)))
		{
			zbx_vector_str_append(values, zbx_strdup(NULL, (char *)val));
			xmlFree(val);
		}
	}

	ret = SUCCEED;
clean:
	if (NULL != xpathObj)
		xmlXPathFreeObject(xpathObj);

	xmlXPathFreeContext(xpathCtx);
	xmlFreeDoc(doc);
out:
	return ret;
}

#endif

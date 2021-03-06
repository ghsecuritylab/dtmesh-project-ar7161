#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#include <net-snmp/agent/scalar.h>

#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include <net-snmp/agent/cache_handler.h>
#include "agent/nsCache.h"
#include "util_funcs.h"



/*
 * OIDs for the cacheging control scalar objects
 *
 * Note that these we're registering the full object rather
 *  than the (sole) valid instance in each case, in order
 *  to handle requests for invalid instances properly.
 */
oid nsCacheTimeout_oid[]    = { 1, 3, 6, 1, 4, 1, 8072, 1, 5, 1};
oid nsCacheEnabled_oid[]    = { 1, 3, 6, 1, 4, 1, 8072, 1, 5, 2};

/*
 * ... and for the cache table.
 */

#define  NSCACHE_TIMEOUT	2
#define  NSCACHE_STATUS		3

#define NSCACHE_STATUS_ENABLED  1
#define NSCACHE_STATUS_DISABLED 2
#define NSCACHE_STATUS_EMPTY    3
#define NSCACHE_STATUS_ACTIVE   4
#define NSCACHE_STATUS_EXPIRED  5

oid nsCacheTable_oid[]      = { 1, 3, 6, 1, 4, 1, 8072, 1, 5, 3};


void
init_nsCache(void)
{
    netsnmp_table_registration_info *table_info;
    netsnmp_iterator_info           *iinfo;

    /*
     * Register the scalar objects...
     */
    DEBUGMSGTL(("nsCacheScalars", "Initializing\n"));
    netsnmp_register_scalar(
        netsnmp_create_handler_registration(
            "nsCacheTimeout", handle_nsCacheTimeout,
            nsCacheTimeout_oid, OID_LENGTH(nsCacheTimeout_oid),
            HANDLER_CAN_RWRITE)
        );
    netsnmp_register_scalar(
        netsnmp_create_handler_registration(
            "nsCacheEnabled", handle_nsCacheEnabled,
            nsCacheEnabled_oid, OID_LENGTH(nsCacheEnabled_oid),
            HANDLER_CAN_RWRITE)
        );

    /*
     * ... and the table.
     * We need to define the column structure and indexing....
     */

    table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);
    if (!table_info) {
        return;
    }
    netsnmp_table_helper_add_indexes(table_info, ASN_PRIV_IMPLIED_OBJECT_ID, 0);
    table_info->min_column = NSCACHE_TIMEOUT;
    table_info->max_column = NSCACHE_STATUS;


    /*
     * .... and the iteration information ....
     */
    iinfo      = SNMP_MALLOC_TYPEDEF(netsnmp_iterator_info);
    if (!iinfo) {
        return;
    }
    iinfo->get_first_data_point = get_first_cache_entry;
    iinfo->get_next_data_point  = get_next_cache_entry;
    iinfo->table_reginfo        = table_info;


    /*
     * .... and register the table with the agent.
     */
    netsnmp_register_table_iterator(
        netsnmp_create_handler_registration(
            "tzCacheTable", handle_nsCacheTable,
            nsCacheTable_oid, OID_LENGTH(nsCacheTable_oid),
            HANDLER_CAN_RWRITE),
        iinfo);
}


/*
 * nsCache scalar handling
 */

int
handle_nsCacheTimeout(netsnmp_mib_handler *handler,
                netsnmp_handler_registration *reginfo,
                netsnmp_agent_request_info *reqinfo,
                netsnmp_request_info *requests)
{
    extern long cache_default_timeout;
    netsnmp_request_info *request=NULL;

    switch (reqinfo->mode) {

    case MODE_GET:
	for (request = requests; request; request=request->next) {
	    snmp_set_var_typed_value(request->requestvb, ASN_INTEGER,
                                     (u_char*)&cache_default_timeout,
                                        sizeof(cache_default_timeout));
	}
	break;


    case MODE_SET_RESERVE1:
	for (request = requests; request; request=request->next) {
            if ( request->status != 0 ) {
                return SNMP_ERR_NOERROR;	/* Already got an error */
            }
            if ( request->requestvb->type != ASN_INTEGER ) {
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGTYPE);
                return SNMP_ERR_WRONGTYPE;
            }
            if ( *request->requestvb->val.integer < 0 ) {
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGVALUE);
                return SNMP_ERR_WRONGVALUE;
            }
        }
        break;

    case MODE_SET_COMMIT:
        cache_default_timeout = *requests->requestvb->val.integer;
        break;
    }

    return SNMP_ERR_NOERROR;
}


int
handle_nsCacheEnabled(netsnmp_mib_handler *handler,
                netsnmp_handler_registration *reginfo,
                netsnmp_agent_request_info *reqinfo,
                netsnmp_request_info *requests)
{
    extern long caching_enabled;
    long enabled;
    netsnmp_request_info *request=NULL;

    switch (reqinfo->mode) {

    case MODE_GET:
	enabled =  (caching_enabled
                       ? NSCACHE_STATUS_ENABLED    /* Actually True/False */
                       : NSCACHE_STATUS_DISABLED );
	for (request = requests; request; request=request->next) {
	    snmp_set_var_typed_value(request->requestvb, ASN_INTEGER,
                                     (u_char*)&enabled, sizeof(enabled));
	}
	break;


    case MODE_SET_RESERVE1:
	for (request = requests; request; request=request->next) {
            if ( request->status != 0 ) {
                return SNMP_ERR_NOERROR;	/* Already got an error */
            }
            if ( request->requestvb->type != ASN_INTEGER ) {
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGTYPE);
                return SNMP_ERR_WRONGTYPE;
            }
            if ((*request->requestvb->val.integer != NSCACHE_STATUS_ENABLED) &&
                (*request->requestvb->val.integer != NSCACHE_STATUS_DISABLED)) {
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGVALUE);
                return SNMP_ERR_WRONGVALUE;
            }
        }
        break;

    case MODE_SET_COMMIT:
        enabled = *requests->requestvb->val.integer;
	if (enabled == NSCACHE_STATUS_DISABLED)
	    enabled = 0;
	caching_enabled = enabled;
        break;
    }

    return SNMP_ERR_NOERROR;
}


/*
 * nsCacheTable handling
 */

netsnmp_variable_list *
get_first_cache_entry(void **loop_context, void **data_context,
                      netsnmp_variable_list *index,
                      netsnmp_iterator_info *data)
{
    extern netsnmp_cache  *cache_head;

    if ( !cache_head )
        return NULL;

    snmp_set_var_value(index, (u_char*)cache_head->rootoid,
		         sizeof(oid) * cache_head->rootoid_len);
    *loop_context = (void*)cache_head;
    *data_context = (void*)cache_head;
    return index;
}

netsnmp_variable_list *
get_next_cache_entry(void **loop_context, void **data_context,
                      netsnmp_variable_list *index,
                      netsnmp_iterator_info *data)
{
    netsnmp_cache *cache = (netsnmp_cache *)*loop_context;
    cache = cache->next;

    if ( !cache )
        return NULL;

    snmp_set_var_value(index, (u_char*)cache->rootoid,
		         sizeof(oid) * cache->rootoid_len);
    *loop_context = (void*)cache;
    *data_context = (void*)cache;
    return index;
}


int
handle_nsCacheTable(netsnmp_mib_handler *handler,
                netsnmp_handler_registration *reginfo,
                netsnmp_agent_request_info *reqinfo,
                netsnmp_request_info *requests)
{
    long status;
    netsnmp_request_info       *request     = NULL;
    netsnmp_table_request_info *table_info  = NULL;
    netsnmp_cache              *cache_entry = NULL;

    switch (reqinfo->mode) {

    case MODE_GET:
        for (request=requests; request; request=request->next) {
            if (requests->processed != 0)
                continue;

            cache_entry = (netsnmp_cache*)netsnmp_extract_iterator_context(request);
            table_info  =                 netsnmp_extract_table_info(request);

            switch (table_info->colnum) {
            case NSCACHE_TIMEOUT:
                if (!cache_entry) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE);
                    continue;
		}
		status = cache_entry->timeout;
	        snmp_set_var_typed_value(request->requestvb, ASN_INTEGER,
                                         (u_char*)&status, sizeof(status));
	        break;

            case NSCACHE_STATUS:
                if (!cache_entry) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE);
                    continue;
		}
		status = (cache_entry->enabled ?
	                   (cache_entry->timestamp ?
                             (!atime_ready(cache_entry->timestamp,
                                          1000*cache_entry->timeout) ?
	                        NSCACHE_STATUS_ACTIVE:
	                        NSCACHE_STATUS_EXPIRED) :
	                      NSCACHE_STATUS_EMPTY) :
	                    NSCACHE_STATUS_DISABLED);
	        snmp_set_var_typed_value(request->requestvb, ASN_INTEGER,
                                         (u_char*)&status, sizeof(status));
	        break;

            default:
                netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT);
                continue;
	    }
	}
	break;


    case MODE_SET_RESERVE1:
        for (request=requests; request; request=request->next) {
            if (requests->processed != 0)
                continue;
            if ( request->status != 0 ) {
                return SNMP_ERR_NOERROR;	/* Already got an error */
            }
            cache_entry = (netsnmp_cache*)netsnmp_extract_iterator_context(request);
            table_info  =                 netsnmp_extract_table_info(request);

            switch (table_info->colnum) {
            case NSCACHE_TIMEOUT:
                if (!cache_entry) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_ERR_NOCREATION);
                    return SNMP_ERR_NOCREATION;
		}
                if ( request->requestvb->type != ASN_INTEGER ) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGTYPE);
                    return SNMP_ERR_WRONGTYPE;
                }
                if (*request->requestvb->val.integer < 0 ) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGVALUE);
                    return SNMP_ERR_WRONGVALUE;
                }
	        break;

            case NSCACHE_STATUS:
                if (!cache_entry) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_ERR_NOCREATION);
                    return SNMP_ERR_NOCREATION;
		}
                if ( request->requestvb->type != ASN_INTEGER ) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGTYPE);
                    return SNMP_ERR_WRONGTYPE;
                }
                status = *request->requestvb->val.integer;
                if (!((status == NSCACHE_STATUS_ENABLED  ) ||
                      (status == NSCACHE_STATUS_DISABLED ) ||
                      (status == NSCACHE_STATUS_EMPTY  ))) {
                    netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGVALUE);
                    return SNMP_ERR_WRONGVALUE;
                }
	        break;

            default:
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_NOCREATION);
                return SNMP_ERR_NOCREATION;	/* XXX - is this right ? */
                continue;
	    }
	}
	break;


    case MODE_SET_COMMIT:
        for (request=requests; request; request=request->next) {
            if (requests->processed != 0)
                continue;
            if ( request->status != 0 ) {
                return SNMP_ERR_NOERROR;	/* Already got an error */
            }
            cache_entry = (netsnmp_cache*)netsnmp_extract_iterator_context(request);
            if (!cache_entry) {
                netsnmp_set_request_error(reqinfo, request, SNMP_ERR_COMMITFAILED);
                return SNMP_ERR_COMMITFAILED;	/* Shouldn't happen! */
            }
            table_info  =                 netsnmp_extract_table_info(request);

            switch (table_info->colnum) {
            case NSCACHE_TIMEOUT:
                cache_entry->timeout = *request->requestvb->val.integer;
	        break;

            case NSCACHE_STATUS:
                switch (*request->requestvb->val.integer) {
                    case NSCACHE_STATUS_ENABLED:
                        cache_entry->enabled = 1;
                        break;
		    case NSCACHE_STATUS_DISABLED:
                        cache_entry->enabled = 0;
                        break;
		    case NSCACHE_STATUS_EMPTY:
                        cache_entry->free_cache(cache_entry, cache_entry->magic);
                        free(cache_entry->timestamp);
                        cache_entry->timestamp = NULL;
                        break;
		}
	        break;
	    }
	}
	break;
    }

    return SNMP_ERR_NOERROR;
}

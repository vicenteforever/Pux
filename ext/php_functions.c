
#include "php.h"
#include "string.h"
#include "pcre.h"
#include "main/php_main.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_API.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "zend_object_handlers.h"
#include "ext/pcre/php_pcre.h"
#include "ext/standard/php_string.h"
#include "php_pux.h"
#include "php_functions.h"
#include "php_expandable_mux.h"

/*
 * pux_match(array $routes, string $path);
 */
PHP_FUNCTION(pux_match)
{
    zval *z_routes;
    char *path;
    int  path_len;

    /* parse parameters */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "as", 
                    &z_routes, 
                    &path, &path_len ) == FAILURE) {
        RETURN_FALSE;
    }

    zval *z_route;
    z_route = php_pux_match(z_routes, path, path_len TSRMLS_CC);
    if ( z_route != NULL ) {
        *return_value = *z_route;
        zval_copy_ctor(return_value);
        return;
    }
    RETURN_NULL();
}


#if ((PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION > 2)) || (PHP_MAJOR_VERSION > 5)
#define PUX_STORE_EG_ENVIRON() \
	{ \
		zval ** __old_return_value_pp   = EG(return_value_ptr_ptr); \
		zend_op ** __old_opline_ptr  	= EG(opline_ptr); \
		zend_op_array * __old_op_array  = EG(active_op_array);

#define PUX_RESTORE_EG_ENVIRON() \
		EG(return_value_ptr_ptr) = __old_return_value_pp;\
		EG(opline_ptr)			 = __old_opline_ptr; \
		EG(active_op_array)		 = __old_op_array; \
	}

#else

#define PUX_STORE_EG_ENVIRON() \
	{ \
		zval ** __old_return_value_pp  		   = EG(return_value_ptr_ptr); \
		zend_op ** __old_opline_ptr 		   = EG(opline_ptr); \
		zend_op_array * __old_op_array 		   = EG(active_op_array); \
		zend_function_state * __old_func_state = EG(function_state_ptr);

#define PUX_RESTORE_EG_ENVIRON() \
		EG(return_value_ptr_ptr) = __old_return_value_pp;\
		EG(opline_ptr)			 = __old_opline_ptr; \
		EG(active_op_array)		 = __old_op_array; \
		EG(function_state_ptr)	 = __old_func_state; \
	}

#endif

int mux_loader(char *path, zval *result TSRMLS_DC) {
    zend_file_handle file_handle;
    zend_op_array   *op_array;
    char realpath[MAXPATHLEN];

    if (!VCWD_REALPATH(path, realpath)) {
        return FAILURE;
    }

    file_handle.filename = path;
    file_handle.free_filename = 0;
    file_handle.type = ZEND_HANDLE_FILENAME;
    file_handle.opened_path = NULL;
    file_handle.handle.fp = NULL;

    op_array = zend_compile_file(&file_handle, ZEND_REQUIRE TSRMLS_CC);

    if (op_array && file_handle.handle.stream.handle) {
        int dummy = 1;

        if (!file_handle.opened_path) {
            file_handle.opened_path = path;
        }

        zend_hash_add(&EG(included_files), file_handle.opened_path, strlen(file_handle.opened_path)+1, (void *)&dummy, sizeof(int), NULL);
    }
    zend_destroy_file_handle(&file_handle TSRMLS_CC);

    if (op_array) {
        zval *local_retval_ptr = NULL;
        PUX_STORE_EG_ENVIRON();
        EG(return_value_ptr_ptr) = &local_retval_ptr;
        EG(active_op_array)      = op_array;

#if ((PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION > 2)) || (PHP_MAJOR_VERSION > 5)
        if (!EG(active_symbol_table)) {
            zend_rebuild_symbol_table(TSRMLS_C);
        }
#endif
        zend_execute(op_array TSRMLS_CC);


        destroy_op_array(op_array TSRMLS_CC);
        efree(op_array);

        if (!EG(exception)) {

            if (EG(return_value_ptr_ptr) && *EG(return_value_ptr_ptr)) {
                if ( result ) {
                    COPY_PZVAL_TO_ZVAL(*result, local_retval_ptr);
                } else {
                    zval_ptr_dtor(EG(return_value_ptr_ptr));
                }
            }
        }
        PUX_RESTORE_EG_ENVIRON();
        return SUCCESS;
    }

    return FAILURE;
}

int _pux_store_mux(char *name, zval * mux TSRMLS_DC) {
    zend_rsrc_list_entry new_le;
    char *persistent_key;
    int ret, persistent_key_len;
    persistent_key_len = spprintf(&persistent_key, 0, "mux_%s", name);
    Z_ADDREF_P(mux);
    new_le.type = le_mux_hash_persist;
    new_le.ptr = mux;
    ret = zend_hash_update(&EG(persistent_list), persistent_key, persistent_key_len + 1, &new_le, sizeof(zend_rsrc_list_entry), NULL);
    efree(persistent_key);
    return ret;
}

zval * _pux_fetch_mux(char *name TSRMLS_DC) {
    zend_rsrc_list_entry *le;
    char *persistent_key;
    int persistent_key_len = spprintf(&persistent_key, 0, "mux_%s", name);
    if ( zend_hash_find(&EG(persistent_list), persistent_key, persistent_key_len + 1, (void**) &le) == SUCCESS) {
        efree(persistent_key);
        return (zval*) le->ptr;
    }
    efree(persistent_key);
    return NULL;
}

PHP_FUNCTION(pux_store_mux)
{
    zval *mux;
    char *name;
    int  name_len;

    /* parse parameters */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz", 
                    &name, &name_len ,
                    &mux ) == FAILURE) {
        RETURN_FALSE;
    }

    if ( _pux_store_mux(name, mux TSRMLS_CC) == SUCCESS ) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}


PHP_FUNCTION(pux_persistent_dispatch)
{
    char *ns, *filename, *path;
    int  ns_len, filename_len, path_len;
    zval *mux = NULL;
    zval *route = NULL;
    zval *z_path = NULL;

    /* parse parameters */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sss", &ns, &ns_len, &filename, &filename_len, &path, &path_len) == FAILURE) {
        RETURN_FALSE;
    }

    ALLOC_INIT_ZVAL(z_path);
    ZVAL_STRINGL(z_path, path ,path_len, 0); // no copy

    mux = _pux_fetch_mux(ns TSRMLS_CC);
    if ( mux == NULL ) {
        ALLOC_INIT_ZVAL(mux);
        if ( mux_loader(filename, mux TSRMLS_CC) == FAILURE ) {
            php_error(E_ERROR, "Can not load Mux object from %s", filename);
        }
        // TODO: compile mux and sort routes
        // zend_print_zval_r(retval, 0 TSRMLS_CC);
        if ( _pux_store_mux(ns, mux TSRMLS_CC) == FAILURE ) {
            php_error(E_ERROR, "Can not store Mux object from %s", filename);
        }
    }

    // do dispatch
    route = call_mux_method(mux, "dispatch" , sizeof("dispatch"), 1 , z_path, NULL, NULL TSRMLS_CC);
    // zval_ptr_dtor(&z_path);
    if ( route ) {
        *return_value = *route;
        zval_copy_ctor(return_value);
        return;
    }
    // route not found
    RETURN_FALSE;
}



PHP_FUNCTION(pux_delete_mux)
{
    char *name, *persistent_key;
    int  name_len, persistent_key_len;
    zend_rsrc_list_entry *le;

    /* parse parameters */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", 
                    &name, &name_len) == FAILURE) {
        RETURN_FALSE;
    }

    persistent_key_len = spprintf(&persistent_key, 0, "mux_%s", name);

    if (zend_hash_del(&EG(persistent_list), persistent_key, persistent_key_len + 1) == SUCCESS) {
        efree(persistent_key);
        RETURN_TRUE;
    }
    efree(persistent_key);
    RETURN_FALSE;
}

PHP_FUNCTION(pux_fetch_mux)
{
    char *name;
    int  name_len;
    zval * mux;

    /* parse parameters */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", 
                    &name, &name_len) == FAILURE) {
        RETURN_FALSE;
    }

    mux = _pux_fetch_mux(name TSRMLS_CC);
    if ( mux ) {
        *return_value = *mux;
        zval_copy_ctor(return_value);
        return;
    }
    RETURN_FALSE;
}

PHP_FUNCTION(pux_sort_routes)
{
    zval *a;
    zval *b;

    /* parse parameters */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", 
                    &a, 
                    &b ) == FAILURE) {
        RETURN_FALSE;
    }

    zval **a_pcre;
    zval **a_pattern;
    zval **a_compiled_pattern;
    zval **a_options;

    zval **b_pcre;
    zval **b_pattern;
    zval **b_compiled_pattern;
    zval **b_options;


    zend_hash_index_find( Z_ARRVAL_P(a) , 0, (void**)&a_pcre);
    zend_hash_index_find( Z_ARRVAL_P(b) , 0, (void**)&b_pcre);

    zend_hash_index_find( Z_ARRVAL_P(a) , 1, (void**)&a_pattern);
    zend_hash_index_find( Z_ARRVAL_P(b) , 1, (void**)&b_pattern);

    zend_hash_index_find( Z_ARRVAL_P(a) , 3, (void**)&a_options);
    zend_hash_index_find( Z_ARRVAL_P(b) , 3, (void**)&b_options);
    

    // return strlen($a[3]['compiled']) > strlen($b[3]['compiled']);
    if ( Z_BVAL_PP(a_pcre) && Z_BVAL_PP(b_pcre) ) {
        zend_hash_find( Z_ARRVAL_PP(a_options) , "compiled", strlen("compiled"), (void**)&a_compiled_pattern);
        zend_hash_find( Z_ARRVAL_PP(b_options) , "compiled", strlen("compiled"), (void**)&b_compiled_pattern);

        int a_len = Z_STRLEN_PP(a_compiled_pattern);
        int b_len = Z_STRLEN_PP(b_compiled_pattern);
        if ( a_len == b_len ) {
            RETURN_LONG(0);
        } else if ( a_len > b_len ) {
            RETURN_LONG(-1);
        } else {
            RETURN_LONG(1);
        }
    }
    else if ( Z_BVAL_PP(a_pcre) ) {
        RETURN_LONG(-1);
    }
    else if ( Z_BVAL_PP(b_pcre) ) {
        RETURN_LONG(1);
    }

    int a_len = Z_STRLEN_PP(a_pattern);
    int b_len = Z_STRLEN_PP(b_pattern);

    if ( a_len == b_len ) {
        RETURN_LONG(0);
    }
    else if ( a_len > b_len ) {
        RETURN_LONG(-1);
    }
    else {
        RETURN_LONG(1);
    }
}


inline int validate_request_method(zval **z_route_options_pp, int current_request_method TSRMLS_DC)
{
    zval **z_route_method = NULL;
    if ( zend_hash_find( Z_ARRVAL_PP(z_route_options_pp) , "method", sizeof("method"), (void**) &z_route_method ) == SUCCESS ) {
        if ( Z_TYPE_PP(z_route_method) == IS_LONG && Z_LVAL_PP(z_route_method) != current_request_method ) {
            return 0;
        }
    }
    return 1;
}

inline int validate_https(zval **z_route_options_pp, int https TSRMLS_DC) 
{
    zval **z_route_secure = NULL;
    if ( zend_hash_find( Z_ARRVAL_PP(z_route_options_pp) , "secure", sizeof("secure"), (void**) &z_route_secure ) == SUCCESS ) {
        // check HTTPS flag
        if ( https && ! Z_BVAL_PP(z_route_secure) ) {
            return 0;
        }
    }
    return 1;
}

inline int validate_domain(zval **z_route_options_pp, zval * http_host TSRMLS_DC) 
{
    zval **z_route_domain = NULL;
    if ( zend_hash_find( Z_ARRVAL_PP(z_route_options_pp) , "domain", sizeof("domain"), (void**) &z_route_domain ) == SUCCESS ) {
        // check HTTP_HOST from $_SERVER
        if ( strncmp(Z_STRVAL_PP(z_route_domain), Z_STRVAL_P(http_host), Z_STRLEN_PP(z_route_domain) ) != 0 ) {
            return 0;
        }
    }
    return 1;
}

// 
// int zend_hash_has_key( )
//
inline zval * php_pux_match(zval *z_routes, char *path, int path_len TSRMLS_DC) {
    int current_request_method;
    int current_https;
    zval * current_http_host;

    zval **server_vars_hash = fetch_server_vars_hash(TSRMLS_C);
    current_request_method = get_current_request_method_const(server_vars_hash TSRMLS_CC);
    current_https          = get_current_https(server_vars_hash TSRMLS_CC);
    current_http_host      = get_current_http_host(server_vars_hash TSRMLS_CC);

    HashPosition z_routes_pointer;

    // for iterating routes
    zval **z_route_pp;

    zval **z_is_pcre_pp; // route[0]
    zval **z_pattern_pp; // route[1]
    // callback @ route[2]
    zval **z_route_options_pp; // route[3]

    pcre_cache_entry *pce;              /* Compiled regular expression */

    zval *pcre_ret = NULL;
    zval *pcre_subpats = NULL; /* Array for subpatterns */

    HashTable * z_routes_hash = Z_ARRVAL_P(z_routes);

    ALLOC_INIT_ZVAL(pcre_ret); // this is required.
    ALLOC_INIT_ZVAL(pcre_subpats); // also required

    for(zend_hash_internal_pointer_reset_ex(z_routes_hash, &z_routes_pointer); 
            zend_hash_get_current_data_ex(z_routes_hash, (void**) &z_route_pp, &z_routes_pointer) == SUCCESS; 
            zend_hash_move_forward_ex(z_routes_hash, &z_routes_pointer)) 
    {
        zend_hash_index_find( Z_ARRVAL_PP(z_route_pp), 0, (void**) &z_is_pcre_pp);
        zend_hash_index_find( Z_ARRVAL_PP(z_route_pp), 1, (void**) &z_pattern_pp);

        if ( Z_BVAL_PP(z_is_pcre_pp) ) {
            /* Compile regex or get it from cache. */
            if ((pce = pcre_get_compiled_regex_cache(Z_STRVAL_PP(z_pattern_pp), Z_STRLEN_PP(z_pattern_pp) TSRMLS_CC)) == NULL) {
                zend_throw_exception(zend_exception_get_default(TSRMLS_C), "PCRE pattern compile failed.", 0 TSRMLS_CC);
                return NULL;
            }

            php_pcre_match_impl(pce, path, path_len, pcre_ret, pcre_subpats, 0, 0, 0, 0 TSRMLS_CC);

            // not matched ?
            if ( ! Z_BVAL_P(pcre_ret) ) {
                continue;
            }

            // tell garbage collector to collect it, we need to use pcre_subpats later.

            // check conditions only when route option is provided
            if ( zend_hash_index_find( Z_ARRVAL_PP(z_route_pp), 3, (void**) &z_route_options_pp) == SUCCESS ) {
                if ( zend_hash_has_more_elements(Z_ARRVAL_PP(z_route_options_pp)) == SUCCESS ) {
                    if ( 0 == validate_request_method( z_route_options_pp, current_request_method TSRMLS_CC) ) {
                        continue;
                    }
                    if ( 0 == validate_https( z_route_options_pp, current_https TSRMLS_CC) ) {
                        continue;
                    }
                    if ( 0 == validate_domain( z_route_options_pp, current_http_host TSRMLS_CC) ) {
                        continue;
                    }
                }
            }

            if ( Z_TYPE_P(pcre_subpats) == IS_NULL ) {
                array_init(pcre_subpats);
            }

            Z_ADDREF_P(pcre_subpats);
            add_assoc_zval(*z_route_options_pp , "vars" , pcre_subpats);
            return *z_route_pp;
            // Apply "default" value to "vars"
            /*
                foreach( $route['variables'] as $k ) {
                    if( isset($regs[$k]) ) {
                        $route['vars'][ $k ] = $regs[$k];
                    } else {
                        $route['vars'][ $k ] = $route['default'][ $k ];
                    }
                }
            */
            /*
            zval **z_route_default;
            zval **z_route_subpat_val;
            if ( zend_hash_find(z_route_options_pp, "default", sizeof("default"), (void**) &z_route_default ) == FAILURE ) {
                HashPosition  default_pointer;
                HashTable    *default_hash;

                variables_hash = Z_ARRVAL_PP(z_variables);

                // foreach variables as var, check if url contains variable or we should apply default value
                for(zend_hash_internal_pointer_reset_ex(variables_hash, &variables_pointer); 
                        zend_hash_get_current_data_ex(variables_hash, (void**) &z_var_name, &variables_pointer) == SUCCESS; 
                        zend_hash_move_forward_ex(variables_hash, &variables_pointer)) 
                {
                }
                // if ( zend_hash_find(z_route_default, "default", sizeof("default"), (void**) &z_route_default ) == FAILURE ) {
            }
            */
        } else {
            // normal string comparison
            // pattern-prefix match
            if ( strncmp(Z_STRVAL_PP( z_pattern_pp ), path, Z_STRLEN_PP( z_pattern_pp )) == 0 ) {
                // check conditions
                if ( zend_hash_index_find( Z_ARRVAL_PP(z_route_pp), 3, (void**) &z_route_options_pp) == SUCCESS ) {
                    if ( zend_hash_num_elements(Z_ARRVAL_PP(z_route_options_pp)) ) {
                        if ( 0 == validate_request_method( z_route_options_pp, current_request_method TSRMLS_CC) ) {
                            continue;
                        }
                        if ( 0 == validate_https( z_route_options_pp, current_https TSRMLS_CC) ) {
                            continue;
                        }
                        if ( 0 == validate_domain( z_route_options_pp, current_http_host TSRMLS_CC) ) {
                            continue;
                        }
                    }
                }
                // we didn't use the pcre variables
                zval_ptr_dtor(&pcre_subpats);
                zval_ptr_dtor(&pcre_ret);
                return *z_route_pp;
            }
        }
    }
    zval_ptr_dtor(&pcre_subpats);
    zval_ptr_dtor(&pcre_ret);
    return NULL;
}

inline zval ** fetch_server_vars_hash(TSRMLS_D) {
    zval **z_server_hash = NULL;
    if ( zend_hash_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER"), (void **) &z_server_hash) == SUCCESS ) {
        return z_server_hash;
    }
    return NULL;
}

inline zval * fetch_server_var(zval ** z_server_hash, char *key , int key_len TSRMLS_DC) {
    zval **rv;
    if ( zend_hash_find(Z_ARRVAL_PP(z_server_hash), key, key_len, (void **) &rv) == SUCCESS ) {
        return *rv;
    }
    return NULL;
}

inline zval * get_current_remote_addr(zval ** server_vars_hash TSRMLS_DC) {
    // REMOTE_ADDR
    return fetch_server_var(server_vars_hash, "REMOTE_ADDR", sizeof("REMOTE_ADDR") TSRMLS_CC);
}

inline zval * get_current_http_host(zval ** server_vars_hash TSRMLS_DC) {
    return fetch_server_var(server_vars_hash, "HTTP_HOST", sizeof("HTTP_HOST") TSRMLS_CC);
}

inline zval * get_current_request_uri(zval ** server_vars_hash TSRMLS_DC) {
    return fetch_server_var(server_vars_hash, "REQUEST_URI", sizeof("REQUEST_URI") TSRMLS_CC);
}

inline int get_current_https(zval ** server_vars_hash TSRMLS_DC) {
    zval *https = fetch_server_var(server_vars_hash, "HTTPS", sizeof("HTTPS") TSRMLS_CC);
    if ( https && Z_BVAL_P(https) ) {
        return 1;
    }
    return 0;
}

inline zval * get_current_request_method(zval ** server_vars_hash TSRMLS_DC) {
    return fetch_server_var(server_vars_hash, "REQUEST_METHOD", sizeof("REQUEST_METHOD") TSRMLS_CC);
}

/**
 * get request method type in constant value.
 */
inline int get_current_request_method_const(zval **server_vars_hash TSRMLS_DC) {
    char *c_request_method;
    zval *z_request_method = get_current_request_method(server_vars_hash TSRMLS_CC);
    if ( z_request_method ) {
        c_request_method = Z_STRVAL_P(z_request_method);
        if ( strncmp("GET", c_request_method , sizeof("GET") ) == 0 ) {
            return REQ_METHOD_GET;
        } else if ( strncmp("POST", c_request_method , sizeof("POST") ) == 0 ) {
            return REQ_METHOD_POST;
        } else if ( strncmp("PUT" , c_request_method , sizeof("PUT") ) == 0 ) {
            return REQ_METHOD_PUT;
        } else if ( strncmp("DELETE", c_request_method, sizeof("DELETE")  ) == 0 ) {
            return REQ_METHOD_DELETE;
        }
    }
    return 0;
}


/* -*- mode: c; indent-tabs-mode: nil -*- */
/*
 * lib/gssapi/krb5/naming_exts.c
 *
 * Copyright 2009 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 *
 */

#include <assert.h>
#include "k5-platform.h"        /* for 64-bit support */
#include "k5-int.h"          /* for zap() */
#include "gssapiP_krb5.h"
#include <stdarg.h>

krb5_error_code
kg_init_name(krb5_context context,
             krb5_principal principal,
             krb5_authdata_context ad_context,
             krb5_flags flags,
             krb5_gss_name_t *name)
{
    krb5_error_code code;

    if (principal == NULL)
        return EINVAL;

    *name = (krb5_gss_name_t)xmalloc(sizeof(krb5_gss_name_rec));
    if (*name == NULL) {
        return ENOMEM;
    }
    (*name)->princ = NULL;
    (*name)->ad_context = NULL;

    if ((flags & KG_INIT_NAME_NO_COPY) == 0) {
        code = krb5_copy_principal(context, principal, &(*name)->princ);
        if (code != 0)
            goto cleanup;

        if (ad_context != NULL) {
            code = krb5_authdata_context_copy(context,
                                              ad_context,
                                              &(*name)->ad_context);
            if (code != 0)
                goto cleanup;
        }
    } else {
        (*name)->princ = principal;
        (*name)->ad_context = ad_context;
    }

    if ((flags & KG_INIT_NAME_INTERN) &&
        !kg_save_name((gss_name_t)*name)) {
        code = G_VALIDATE_FAILED;
        goto cleanup;
    }

    code = 0;

cleanup:
    if (code != 0) {
        if (*name != NULL) {
            krb5_free_principal(context, (*name)->princ);
            krb5_authdata_context_free(context, (*name)->ad_context);
            free(*name);
            *name = NULL;
        }
    }

    return code;
}

krb5_error_code
kg_release_name(krb5_context context,
                krb5_gss_name_t *name)
{
    if (*name != NULL) {
        if ((*name)->princ != NULL)
            krb5_free_principal(context, (*name)->princ);
        if ((*name)->ad_context != NULL)
            krb5_authdata_context_free(context, (*name)->ad_context);
        free(*name);
        *name = NULL;
    }

    return 0;
}

krb5_error_code
kg_duplicate_name(krb5_context context,
                  const krb5_gss_name_t src,
                  krb5_flags flags,
                  krb5_gss_name_t *dst)
{
    return kg_init_name(context, src->princ,
                        src->ad_context, flags, dst);
}


krb5_boolean
kg_compare_name(krb5_context context,
                krb5_gss_name_t name1,
                krb5_gss_name_t name2)
{
    return krb5_principal_compare(context, name1->princ, name2->princ);
}

static OM_uint32
kg_map_name_error(OM_uint32 *minor_status, krb5_error_code code)
{
    OM_uint32 major_status;

    switch (code) {
    case 0:
        major_status = GSS_S_COMPLETE;
        break;
    case ENOENT:
    case EPERM:
        major_status = GSS_S_UNAVAILABLE;
        break;
    default:
        major_status = GSS_S_FAILURE;
        break;
    }

    *minor_status = code;

    return major_status;
}

/* Owns data on success */
static krb5_error_code
kg_data_list_to_buffer_set_nocopy(krb5_data **pdata,
                                  gss_buffer_set_t *buffer_set)
{
    gss_buffer_set_t set;
    OM_uint32 minor_status;
    unsigned int i;
    krb5_data *data;

    data = *pdata;

    if (data == NULL) {
        *buffer_set = GSS_C_NO_BUFFER_SET;
        return 0;
    }

    if (GSS_ERROR(gss_create_empty_buffer_set(&minor_status,
                                              &set)))
        return minor_status;

    for (i = 0; data[i].data != NULL; i++)
        ;

    set->count = i;
    set->elements = calloc(i, sizeof(gss_buffer_desc));
    if (set->elements == NULL) {
        gss_release_buffer_set(&minor_status, &set);
        return ENOMEM;
    }

    for (i = 0; i < set->count; i++) {
        set->elements[i].length = data[i].length;
        set->elements[i].value = data[i].data;
    }

    free(data);
    *pdata = NULL;

    *buffer_set = set;

    return 0;
}

OM_uint32
krb5_gss_inquire_name(OM_uint32 *minor_status,
                      gss_name_t name,
                      int name_is_MN,
                      gss_OID *MN_mech,
                      gss_buffer_set_t *authenticated,
                      gss_buffer_set_t *asserted,
                      gss_buffer_set_t *complete)
{
    krb5_context context;
    krb5_error_code code;
    krb5_gss_name_t kname;
    krb5_data *kauthenticated = NULL;
    krb5_data *kasserted = NULL;
    krb5_data *kcomplete = NULL;

    if (minor_status != NULL)
        *minor_status = 0;

    *authenticated = GSS_C_NO_BUFFER_SET;
    *asserted = GSS_C_NO_BUFFER_SET;
    *complete = GSS_C_NO_BUFFER_SET;

    code = krb5_gss_init_context(&context);
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }

    if (!kg_validate_name(name)) {
        *minor_status = (OM_uint32)G_VALIDATE_FAILED;
        krb5_free_context(context);
        return GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME;
    }

    kname = (krb5_gss_name_t)name;
    if (kname->ad_context == NULL) {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    code = krb5_authdata_get_attribute_types(context,
                                             kname->ad_context,
                                             &kasserted,
                                             &kauthenticated);
    if (code != 0)
        goto cleanup;

    code = kg_data_list_to_buffer_set_nocopy(&kasserted,
                                             asserted);
    if (code != 0)
        goto cleanup;

    code = kg_data_list_to_buffer_set_nocopy(&kauthenticated,
                                             authenticated);
    if (code != 0)
        goto cleanup;

cleanup:
    krb5int_free_data_list(context, kasserted);
    krb5int_free_data_list(context, kauthenticated);

    krb5_free_context(context);

    return kg_map_name_error(minor_status, code);
}

OM_uint32
krb5_gss_get_name_attribute(OM_uint32 *minor_status,
                            gss_name_t name,
                            gss_buffer_t attr,
                            int *authenticated,
                            int *complete,
                            gss_buffer_t value,
                            gss_buffer_t display_value,
                            int *more)
{
    krb5_context context;
    krb5_error_code code;
    krb5_gss_name_t kname;
    krb5_data kattr;
    krb5_boolean kauthenticated;
    krb5_boolean kcomplete;
    krb5_data kvalue;
    krb5_data kdisplay_value;

    if (minor_status != NULL)
        *minor_status = 0;

    code = krb5_gss_init_context(&context);
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }

    if (!kg_validate_name(name)) {
        *minor_status = (OM_uint32)G_VALIDATE_FAILED;
        krb5_free_context(context);
        return GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME;
    }

    kname = (krb5_gss_name_t)name;
    if (kname->ad_context == NULL) {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    kattr.data = (char *)attr->value;
    kattr.length = attr->length;

    kauthenticated = FALSE;
    kcomplete = FALSE;

    code = krb5_authdata_get_attribute(context,
                                       kname->ad_context,
                                       &kattr,
                                       &kauthenticated,
                                       &kcomplete,
                                       &kvalue,
                                       &kdisplay_value,
                                       more);
    if (code == 0) {
        value->value = kvalue.data;
            value->length = kvalue.length;

        *authenticated = kauthenticated;
        *complete = kcomplete;

        display_value->value = kvalue.data;
        display_value->length = kvalue.length;
    }

    krb5_free_context(context);

    return kg_map_name_error(minor_status, code);
}

OM_uint32
krb5_gss_set_name_attribute(OM_uint32 *minor_status,
                            gss_name_t name,
                            int complete,
                            gss_buffer_t attr,
                            gss_buffer_t value)
{
    krb5_context context;
    krb5_error_code code;
    krb5_gss_name_t kname;
    krb5_data kattr;
    krb5_data kvalue;

    if (minor_status != NULL)
        *minor_status = 0;

    code = krb5_gss_init_context(&context);
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }

    if (!kg_validate_name(name)) {
        *minor_status = (OM_uint32)G_VALIDATE_FAILED;
        krb5_free_context(context);
        return GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME;
    }

    kname = (krb5_gss_name_t)name;
    if (kname->ad_context == NULL) {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    kattr.data = (char *)attr->value;
    kattr.length = attr->length;

    kvalue.data = (char *)value->value;
    kvalue.length = attr->length;

    code = krb5_authdata_set_attribute(context,
                                       kname->ad_context,
                                       complete,
                                       &kattr,
                                       &kvalue);

    krb5_free_context(context);

    return kg_map_name_error(minor_status, code);
}

OM_uint32
krb5_gss_delete_name_attribute(OM_uint32 *minor_status,
                               gss_name_t name,
                               gss_buffer_t attr)
{
    krb5_context context;
    krb5_error_code code;
    krb5_gss_name_t kname;
    krb5_data kattr;

    if (minor_status != NULL)
        *minor_status = 0;

    code = krb5_gss_init_context(&context);
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }

    if (!kg_validate_name(name)) {
        *minor_status = (OM_uint32)G_VALIDATE_FAILED;
        krb5_free_context(context);
        return GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME;
    }

    kname = (krb5_gss_name_t)name;
    if (kname->ad_context == NULL) {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    kattr.data = (char *)attr->value;
    kattr.length = attr->length;

    code = krb5_authdata_delete_attribute(context,
                                          kname->ad_context,
                                          &kattr);

    krb5_free_context(context);

    return kg_map_name_error(minor_status, code);
}

OM_uint32
krb5_gss_map_name_to_any(OM_uint32 *minor_status,
                         gss_name_t name,
                         int authenticated,
                         gss_buffer_t type_id,
                         gss_any_t *output)
{
    krb5_context context;
    krb5_error_code code;
    krb5_gss_name_t kname;
    char *kmodule;

    if (minor_status != NULL)
        *minor_status = 0;

    code = krb5_gss_init_context(&context);
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }

    if (!kg_validate_name(name)) {
        *minor_status = (OM_uint32)G_VALIDATE_FAILED;
        krb5_free_context(context);
        return GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME;
    }

    kname = (krb5_gss_name_t)name;
    if (kname->ad_context == NULL) {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    kmodule = (char *)type_id->value;
    if (kmodule[type_id->length] != '\0') {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    code = krb5_authdata_export_internal(context,
                                         kname->ad_context,
                                         kmodule,
                                         (void **)output);

    krb5_free_context(context);

    return kg_map_name_error(minor_status, code);
}

OM_uint32
krb5_gss_release_any_name_mapping(OM_uint32 *minor_status,
                                  gss_name_t name,
                                  gss_buffer_t type_id,
                                  gss_any_t *input)
{
    krb5_context context;
    krb5_error_code code;
    krb5_gss_name_t kname;
    char *kmodule;

    if (minor_status != NULL)
        *minor_status = 0;

    code = krb5_gss_init_context(&context);
    if (code != 0) {
        *minor_status = code;
        return GSS_S_FAILURE;
    }

    if (!kg_validate_name(name)) {
        *minor_status = (OM_uint32)G_VALIDATE_FAILED;
        krb5_free_context(context);
        return GSS_S_CALL_BAD_STRUCTURE|GSS_S_BAD_NAME;
    }

    kname = (krb5_gss_name_t)name;
    if (kname->ad_context == NULL) {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    kmodule = (char *)type_id->value;
    if (kmodule[type_id->length] != '\0') {
        krb5_free_context(context);
        return GSS_S_UNAVAILABLE;
    }

    code = krb5_authdata_free_internal(context,
                                       kname->ad_context,
                                       kmodule,
                                       *input);
    if (code == 0)
        *input = NULL;

    krb5_free_context(context);

    return kg_map_name_error(minor_status, code);

}

#if 0
OM_uint32
krb5_gss_export_name_composite(OM_uint32 *minor_status,
                               gss_name_t name,
                               gss_buffer_t exp_composite_name)
{
}

OM_uint32
krb5_gss_display_name_ext(OM_uint32 *minor_status,
                          gss_name_t name,
                          gss_OID display_as_name_type,
                          gss_buffer_t display_name)
{
}

#endif

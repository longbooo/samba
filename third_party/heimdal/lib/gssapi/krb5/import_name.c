/*
 * Copyright (c) 1997 - 2003 Kungliga Tekniska Högskolan
 * (Royal Institute of Technology, Stockholm, Sweden).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "gsskrb5_locl.h"

static OM_uint32
parse_krb5_name (OM_uint32 *minor_status,
		 krb5_context context,
		 const char *name,
		 gss_name_t *output_name)
{
    krb5_principal princ;
    krb5_error_code kerr;

    kerr = krb5_parse_name (context, name, &princ);

    if (kerr == 0) {
	*output_name = (gss_name_t)princ;
	return GSS_S_COMPLETE;
    }
    *minor_status = kerr;

    if (kerr == KRB5_PARSE_ILLCHAR || kerr == KRB5_PARSE_MALFORMED)
	return GSS_S_BAD_NAME;

    return GSS_S_FAILURE;
}

static OM_uint32
import_krb5_name (OM_uint32 *minor_status,
		  krb5_context context,
		  const gss_buffer_t input_name_buffer,
		  gss_name_t *output_name)
{
    OM_uint32 ret;
    char *tmp;

    tmp = malloc (input_name_buffer->length + 1);
    if (tmp == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    memcpy (tmp,
	    input_name_buffer->value,
	    input_name_buffer->length);
    tmp[input_name_buffer->length] = '\0';

    ret = parse_krb5_name(minor_status, context, tmp, output_name);
    free(tmp);

    return ret;
}

OM_uint32
_gsskrb5_canon_name(OM_uint32 *minor_status, krb5_context context,
		    gss_const_name_t targetname, krb5_principal *out)
{
    krb5_const_principal p = (krb5_const_principal)targetname;
    krb5_error_code ret;
    char *hostname = NULL, *service;
    int type;
    const char *comp;

    *minor_status = 0;

    /* If its not a hostname */
    type = krb5_principal_get_type(context, p);
    comp = krb5_principal_get_comp_string(context, p, 0);
    if (type == KRB5_NT_SRV_HST || type == KRB5_NT_SRV_HST_NEEDS_CANON ||
	(type == KRB5_NT_UNKNOWN && comp != NULL && strcmp(comp, "host") == 0)) {
	if (p->name.name_string.len == 0)
	    return GSS_S_BAD_NAME;
	else if (p->name.name_string.len > 1)
	    hostname = p->name.name_string.val[1];

	service = p->name.name_string.val[0];

	ret = krb5_sname_to_principal(context,
				      hostname,
				      service,
				      KRB5_NT_SRV_HST,
				      out);
	if (ret == 0) {
	    const char *in_realm = krb5_principal_get_realm(context,
							    p);
	    const char *out_realm = krb5_principal_get_realm(context,
							     *out);

	    /*
	     * Avoid loss of information, check for the "referral
	     * realm" and set back what was specified.
	     */
	    if (out_realm != NULL && out_realm[0] == '\0') {
		ret = krb5_principal_set_realm(context, *out, in_realm);
	    }
	}
    } else {
	ret = krb5_copy_principal(context, p, out);
    }

    if (ret) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    return 0;
}


static OM_uint32
import_hostbased_name(OM_uint32 *minor_status,
		      krb5_context context,
		      const gss_buffer_t input_name_buffer,
		      gss_name_t *output_name)
{
    krb5_principal princ = NULL;
    krb5_error_code kerr;
    char *tmp, *p, *host = NULL;

    tmp = malloc (input_name_buffer->length + 1);
    if (tmp == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    memcpy (tmp,
	    input_name_buffer->value,
	    input_name_buffer->length);
    tmp[input_name_buffer->length] = '\0';

    p = strchr (tmp, '@');
    if (p != NULL) {
	*p = '\0';
	host = p + 1;
    }

    kerr = krb5_make_principal(context, &princ, "", tmp, host, NULL);
    free (tmp);
    *minor_status = kerr;
    if (kerr == KRB5_PARSE_ILLCHAR || kerr == KRB5_PARSE_MALFORMED)
	return GSS_S_BAD_NAME;
    else if (kerr)
	return GSS_S_FAILURE;

    krb5_principal_set_type(context, princ, KRB5_NT_SRV_HST);
    *output_name = (gss_name_t)princ;

    return 0;
}

static OM_uint32
import_export_name (OM_uint32 *minor_status,
		    krb5_context context,
		    const gss_buffer_t input_name_buffer,
		    gss_name_t *output_name)
{
    unsigned char *p;
    uint32_t length;
    OM_uint32 ret;
    char *name;

    if (input_name_buffer->length < 10 + GSS_KRB5_MECHANISM->length)
	return GSS_S_BAD_NAME;

    /* TOK, MECH_OID_LEN, DER(MECH_OID), NAME_LEN, NAME */

    p = input_name_buffer->value;

    if (memcmp(&p[0], "\x04\x01\x00", 3) != 0 ||
	p[3] != GSS_KRB5_MECHANISM->length + 2 ||
	p[4] != 0x06 ||
	p[5] != GSS_KRB5_MECHANISM->length ||
	memcmp(&p[6], GSS_KRB5_MECHANISM->elements,
	       GSS_KRB5_MECHANISM->length) != 0)
	return GSS_S_BAD_NAME;

    p += 6 + GSS_KRB5_MECHANISM->length;

    length = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    p += 4;

    if (length > input_name_buffer->length - 10 - GSS_KRB5_MECHANISM->length)
	return GSS_S_BAD_NAME;

    name = malloc(length + 1);
    if (name == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    memcpy(name, p, length);
    name[length] = '\0';

    ret = parse_krb5_name(minor_status, context, name, output_name);
    free(name);

    return ret;
}

OM_uint32 GSSAPI_CALLCONV _gsskrb5_import_name
           (OM_uint32 * minor_status,
            const gss_buffer_t input_name_buffer,
            const gss_OID input_name_type,
            gss_name_t * output_name
           )
{
    krb5_context context;

    *minor_status = 0;
    *output_name = GSS_C_NO_NAME;

    GSSAPI_KRB5_INIT (&context);

    if (gss_oid_equal(input_name_type, GSS_C_NT_HOSTBASED_SERVICE) ||
	gss_oid_equal(input_name_type, GSS_C_NT_HOSTBASED_SERVICE_X))
	return import_hostbased_name (minor_status,
				      context,
				      input_name_buffer,
				      output_name);
    else if (input_name_type == GSS_C_NO_OID
	     || gss_oid_equal(input_name_type, GSS_C_NT_USER_NAME)
	     || gss_oid_equal(input_name_type, GSS_KRB5_NT_PRINCIPAL_NAME))
 	/* default printable syntax */
	return import_krb5_name (minor_status,
				 context,
				 input_name_buffer,
				 output_name);
    else if (gss_oid_equal(input_name_type, GSS_C_NT_EXPORT_NAME)) {
	return import_export_name(minor_status,
				  context,
				  input_name_buffer,
				  output_name);
    } else {
	*minor_status = 0;
	return GSS_S_BAD_NAMETYPE;
    }
}

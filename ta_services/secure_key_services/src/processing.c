// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2017-2018, Linaro Limited
 */

#include <assert.h>
#include <sks_internal_abi.h>
#include <sks_ta.h>
#include <string.h>
#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <util.h>

#include "attributes.h"
#include "object.h"
#include "pkcs11_token.h"
#include "pkcs11_attributes.h"
#include "processing.h"
#include "serializer.h"
#include "sks_helpers.h"

static void release_active_processing(struct pkcs11_session *session)
{
	switch (session->proc_id) {
	case SKS_CKM_AES_CTR:
		tee_release_ctr_operation(session);
		break;
	case SKS_CKM_AES_GCM:
		tee_release_gcm_operation(session);
		break;
	case SKS_CKM_AES_CCM:
		tee_release_ccm_operation(session);
		break;
	default:
		break;
	}

	session->proc_id = SKS_UNDEFINED_ID;

	if (session->tee_op_handle != TEE_HANDLE_NULL) {
		TEE_FreeOperation(session->tee_op_handle);
		session->tee_op_handle = TEE_HANDLE_NULL;
	}

	if (set_processing_state(session, PKCS11_SESSION_READY))
		TEE_Panic(0);
}

uint32_t entry_import_object(uintptr_t tee_session,
			     TEE_Param *ctrl, TEE_Param *in, TEE_Param *out)
{
	uint32_t rv;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	struct pkcs11_session *session;
	struct sks_attrs_head *head = NULL;
	struct sks_object_head *template = NULL;
	size_t template_size;
	uint32_t obj_handle;

	/*
	 * Collect the arguments of the request
	 */

	if (!ctrl || in || !out)
		return SKS_BAD_PARAM;

	if (out->memref.size < sizeof(uint32_t)) {
		out->memref.size = sizeof(uint32_t);
		return SKS_SHORT_BUFFER;
	}

	if ((uintptr_t)out->memref.buffer & 0x3UL)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	rv = serialargs_alloc_get_attributes(&ctrlargs, &template);
	if (rv)
		return rv;

	template_size = sizeof(*template) + template->attrs_size;

	/* Check session/token state against object import */
	session = sks_handle2session(session_handle, tee_session);
	if (!session) {
		rv = SKS_CKR_SESSION_HANDLE_INVALID;
		goto bail;
	}

	if (check_processing_state(session, PKCS11_SESSION_READY)) {
		rv = SKS_CKR_OPERATION_ACTIVE;
		goto bail;
	}

	/*
	 * Prepare a clean initial state for the requested object attributes.
	 * Free temorary template once done.
	 */
	rv = create_attributes_from_template(&head, template, template_size,
					     NULL, SKS_FUNCTION_IMPORT);
	TEE_Free(template);
	template = NULL;
	if (rv)
		goto bail;

	/*
	 * Check target object attributes match target processing
	 * Check target object attributes match token state
	 */
	rv = check_created_attrs_against_processing(SKS_PROCESSING_IMPORT,
						    head);
	if (rv)
		goto bail;

	rv = check_created_attrs_against_token(session, head);
	if (rv)
		goto bail;

	/*
	 * Execute the target processing and add value as attribute SKS_CKA_VALUE.
	 * Raw import => key value in clear already as attribute SKS_CKA_VALUE.
	 *
	 * Here we only check attribute that attribute SKS_CKA_VALUE is defined.
	 * TODO: check value size? check SKS_CKA_VALUE_LEN? check SKS_CHECKSUM.
	 */
	rv = get_attribute_ptr(head, SKS_CKA_VALUE, NULL, NULL);
	if (rv)
		goto bail;

	/*
	 * At this stage the object is almost created: all its attributes are
	 * referenced in @head, including the key value and are assume
	 * reliable. Now need to register it and get a handle for it.
	 */
	rv = create_object(session, head, &obj_handle);
	if (rv)
		goto bail;

	/*
	 * Now obj_handle (through the related struct sks_object instance)
	 * owns the serialised buffer that holds the object attributes.
	 * We reset attrs->buffer to NULL as serializer object is no more
	 * the attributes buffer owner.
	 */
	head = NULL;

	TEE_MemMove(out->memref.buffer, &obj_handle, sizeof(uint32_t));
	out->memref.size = sizeof(uint32_t);

bail:
	TEE_Free(template);
	TEE_Free(head);

	return rv;
}

/*
 * Get the GPD TEE cipher operation parameters (mode, key size, algo)
 * from and SKS cipher operation.
 */
static uint32_t tee_operarion_params(struct pkcs11_session *session,
					struct sks_attribute_head *proc_params,
					struct sks_object *sks_key,
					uint32_t function)
{
	uint32_t key_type;
	uint32_t algo;
	uint32_t mode;
	uint32_t size;
	TEE_Result res;
	void *value;
	size_t value_size;

	if (get_attribute_ptr(sks_key->attributes, SKS_CKA_VALUE,
				&value, &value_size))
		TEE_Panic(0);

	if (get_attribute(sks_key->attributes, SKS_CKA_KEY_TYPE,
			  &key_type, NULL))
		return SKS_ERROR;

	switch (key_type) {
	case SKS_CKK_AES:

		if (function == SKS_FUNCTION_ENCRYPT ||
		    function == SKS_FUNCTION_DECRYPT) {
			mode = (function == SKS_FUNCTION_DECRYPT) ?
					TEE_MODE_DECRYPT : TEE_MODE_ENCRYPT;
			size = value_size * 8;

			switch (proc_params->id) {
			case SKS_CKM_AES_ECB:
				algo = TEE_ALG_AES_ECB_NOPAD;
				break;
			case SKS_CKM_AES_CBC:
				algo = TEE_ALG_AES_CBC_NOPAD;
				break;
			case SKS_CKM_AES_CTR:
				algo = TEE_ALG_AES_CTR;
				break;
			case SKS_CKM_AES_CTS:
				algo = TEE_ALG_AES_CTS;
				break;
			case SKS_CKM_AES_CCM:
				algo = TEE_ALG_AES_CCM;
				break;
			case SKS_CKM_AES_GCM:
				algo = TEE_ALG_AES_GCM;
				break;
			default:
				EMSG("Operation not supported for process %s",
					sks2str_proc(proc_params->id));
				return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
			}
			break;
		}

		if (function == SKS_FUNCTION_SIGN ||
		    function == SKS_FUNCTION_VERIFY) {

			switch (proc_params->id) {
			case SKS_CKM_AES_CMAC:
			case SKS_CKM_AES_CMAC_GENERAL:
				algo = TEE_ALG_AES_CMAC;
				mode = TEE_MODE_MAC;
				size = value_size * 8;
				break;

			case SKS_CKM_AES_XCBC_MAC:
				algo = TEE_ALG_AES_CBC_MAC_NOPAD;
				mode = TEE_MODE_MAC;
				size = value_size * 8;
				break;

			default:
				EMSG("Operation not supported for process %s",
					sks2str_proc(proc_params->id));
				return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
			}
			break;
		}

		EMSG("Operation not supported for object type %s",
			sks2str_key_type(key_type));
		return SKS_FAILED;

	case SKS_CKK_GENERIC_SECRET:
	case SKS_CKK_MD5_HMAC:
	case SKS_CKK_SHA_1_HMAC:
	case SKS_CKK_SHA224_HMAC:
	case SKS_CKK_SHA256_HMAC:
	case SKS_CKK_SHA384_HMAC:
	case SKS_CKK_SHA512_HMAC:
		if (function == SKS_FUNCTION_SIGN ||
		    function == SKS_FUNCTION_VERIFY) {

			mode = TEE_MODE_MAC;
			size = value_size * 8;

			switch (proc_params->id) {
			case SKS_CKM_MD5_HMAC:
				algo = TEE_ALG_HMAC_MD5;
				if (key_type != SKS_CKK_GENERIC_SECRET &&
				   key_type != SKS_CKK_MD5_HMAC)
					return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			case SKS_CKM_SHA_1_HMAC:
				algo = TEE_ALG_HMAC_SHA1;
				if (key_type != SKS_CKK_GENERIC_SECRET &&
				   key_type != SKS_CKK_SHA_1_HMAC)
					return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			case SKS_CKM_SHA224_HMAC:
				algo = TEE_ALG_HMAC_SHA224;
				if (key_type != SKS_CKK_GENERIC_SECRET &&
				   key_type != SKS_CKK_SHA224_HMAC)
					return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			case SKS_CKM_SHA256_HMAC:
				algo = TEE_ALG_HMAC_SHA256;
				if (key_type != SKS_CKK_GENERIC_SECRET &&
				   key_type != SKS_CKK_SHA256_HMAC)
					return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			case SKS_CKM_SHA384_HMAC:
				algo = TEE_ALG_HMAC_SHA384;
				if (key_type != SKS_CKK_GENERIC_SECRET &&
				   key_type != SKS_CKK_SHA384_HMAC)
					return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			case SKS_CKM_SHA512_HMAC:
				algo = TEE_ALG_HMAC_SHA512;
				if (key_type != SKS_CKK_GENERIC_SECRET &&
				   key_type != SKS_CKK_SHA512_HMAC)
					return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
				break;
			default:
				EMSG("Operation not supported for process %s",
					sks2str_proc(proc_params->id));
				return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
			}
			break;
		}

		EMSG("Operation not supported for object type %s",
			sks2str_key_type(key_type));
		return SKS_FAILED;

	default:
		EMSG("Operation not supported for object type %s",
			sks2str_key_type(key_type));
		return SKS_FAILED;
	}

	if (session->tee_op_handle != TEE_HANDLE_NULL)
		TEE_Panic(0);

	res = TEE_AllocateOperation(&session->tee_op_handle, algo, mode, size);
	if (res) {
		EMSG("Failed to allocate operation");
		return tee2sks_error(res);
	}

	return SKS_OK;
}

/* Convert SKS_CKK_xxx into GPD TEE_ATTR_xxx */
static uint32_t get_tee_object_info(uint32_t *type, uint32_t *attr,
				    struct sks_attrs_head *head)
{
	//
	// TODO: SKS_CKK_GENERIC_SECRET should be allowed to be used for HMAC_SHAx
	//
	switch (get_type(head)) {
	case SKS_CKK_AES:
		*type = TEE_TYPE_AES;
		goto secret;
	case SKS_CKK_GENERIC_SECRET:
		*type = TEE_TYPE_GENERIC_SECRET;
		goto secret;
	case SKS_CKK_MD5_HMAC:
		*type = TEE_TYPE_HMAC_MD5;
		goto secret;
	case SKS_CKK_SHA_1_HMAC:
		*type = TEE_TYPE_HMAC_SHA1;
		goto secret;
	case SKS_CKK_SHA224_HMAC:
		*type = TEE_TYPE_HMAC_SHA224;
		goto secret;
	case SKS_CKK_SHA256_HMAC:
		*type = TEE_TYPE_HMAC_SHA256;
		goto secret;
	case SKS_CKK_SHA384_HMAC:
		*type = TEE_TYPE_HMAC_SHA384;
		goto secret;
	case SKS_CKK_SHA512_HMAC:
		*type = TEE_TYPE_HMAC_SHA512;
		goto secret;
	default:
		EMSG("Operation not supported for object type %s",
			sks2str_key_type(get_type(head)));
		return SKS_CKR_ATTRIBUTE_TYPE_INVALID;
	}

secret:
	*attr = TEE_ATTR_SECRET_VALUE;
	return SKS_OK;
}

static uint32_t load_key(struct sks_object *obj)
{
	uint32_t tee_obj_type;
	uint32_t tee_obj_attr;
	TEE_Attribute tee_key_attr;
	void *value;
	size_t value_size;
	uint32_t rv;
	TEE_Result res;

	/* Key already loaded, we have a handle */
	if (obj->key_handle != TEE_HANDLE_NULL)
		return SKS_OK;

	rv = get_tee_object_info(&tee_obj_type, &tee_obj_attr,
				 obj->attributes);
	if (rv) {
		EMSG("get_tee_object_info failed, %s", sks2str_rc(rv));
		return rv;
	}

	if (get_attribute_ptr(obj->attributes, SKS_CKA_VALUE,
			      &value, &value_size))
		TEE_Panic(0);

	res = TEE_AllocateTransientObject(tee_obj_type, value_size * 8,
					  &obj->key_handle);
	if (res) {
		EMSG("TEE_AllocateTransientObject failed, %" PRIx32, res);
		return rv;;
	}

	TEE_InitRefAttribute(&tee_key_attr, tee_obj_attr, value, value_size);

	res = TEE_PopulateTransientObject(obj->key_handle, &tee_key_attr, 1);
	if (res) {
		EMSG("TEE_PopulateTransientObject failed, %" PRIx32, res);
		TEE_FreeTransientObject(obj->key_handle);
		obj->key_handle = TEE_HANDLE_NULL;
		return tee2sks_error(res);
	}

	return rv;
}

/*
 * ctrl = [session-handle][key-handle][mechanism-parameters]
 * in = none
 * out = none
 */
uint32_t entry_cipher_init(uintptr_t tee_session, TEE_Param *ctrl,
			   TEE_Param *in, TEE_Param *out, int decrypt)
{
	uint32_t rv;
	TEE_Result res;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	uint32_t key_handle;
	struct sks_attribute_head *proc_params = NULL;
	struct sks_object *obj;
	struct pkcs11_session *session = NULL;

	if (!ctrl || in || out)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	rv = serialargs_get(&ctrlargs, &key_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	rv = serialargs_alloc_get_one_attribute(&ctrlargs, &proc_params);
	if (rv)
		return rv;

	/*
	 * Check PKCS session (arguments and session state)
	 */
	session = sks_handle2session(session_handle, tee_session);
	if (!session) {
		rv = SKS_CKR_SESSION_HANDLE_INVALID;
		goto bail;
	}

	if (check_processing_state(session, PKCS11_SESSION_READY)) {
		rv = SKS_CKR_OPERATION_ACTIVE;
		goto bail;
	}

	if (set_processing_state(session, decrypt ?
				 PKCS11_SESSION_DECRYPTING :
				 PKCS11_SESSION_ENCRYPTING)) {
		rv = SKS_CKR_OPERATION_ACTIVE;
		goto bail;
	}

	/*
	 * Check parent key handle
	 */
	obj = sks_handle2object(key_handle, session);
	if (!obj) {
		rv = SKS_CKR_KEY_HANDLE_INVALID;
		goto bail;
	}

	/*
	 * Check processing against parent key and token state
	 */
	rv = check_parent_attrs_against_processing(proc_params->id, decrypt ?
						   SKS_FUNCTION_DECRYPT :
						   SKS_FUNCTION_ENCRYPT,
						   obj->attributes);
	if (rv)
		goto bail;

	rv = check_parent_attrs_against_token(session, obj->attributes);
	if (rv)
		goto bail;

	/*
	 * Allocate a TEE operation for the target processing and
	 * fill it with the expected operation parameters.
	 */
	rv = tee_operarion_params(session, proc_params, obj, decrypt ?
				  SKS_FUNCTION_DECRYPT : SKS_FUNCTION_ENCRYPT);
	if (rv)
		goto bail;


	/*
	 * Create a TEE object from the target key, if not yet done
	 */
	switch (get_class(obj->attributes)) {
	case SKS_CKO_SECRET_KEY:
		rv = load_key(obj);
		if (rv)
			goto bail;

		break;

	default:
		rv = SKS_FAILED;		// FIXME: errno
		goto bail;
	}

	res = TEE_SetOperationKey(session->tee_op_handle, obj->key_handle);
	if (res) {
		EMSG("TEE_SetOperationKey failed %x", res);
		rv = tee2sks_error(res);
		goto bail;
	}

	/*
	 * Specifc cipher initialization if any
	 */
	switch (proc_params->id) {
	case SKS_CKM_AES_ECB:
		if (proc_params->size) {
			DMSG("Bad params for %s", sks2str_proc(proc_params->id));
			rv = SKS_CKR_MECHANISM_PARAM_INVALID;
			goto bail;
		}

		TEE_CipherInit(session->tee_op_handle, NULL, 0);
		break;

	case SKS_CKM_AES_CBC:
	case SKS_CKM_AES_CBC_PAD:
	case SKS_CKM_AES_CTS:
		if (proc_params->size != 16) {
			DMSG("Expects 16 byte IV, not %d", proc_params->size);
			rv = SKS_CKR_MECHANISM_PARAM_INVALID;
			goto bail;
		}

		TEE_CipherInit(session->tee_op_handle,
				(void *)proc_params->data, 16);
		break;

	case SKS_CKM_AES_CTR:
		rv = tee_init_ctr_operation(session,
					    proc_params->data,
					    proc_params->size);
		if (rv)
			goto bail;
		break;

	case SKS_CKM_AES_CCM:
		rv = tee_init_ccm_operation(session,
					    proc_params->data,
					    proc_params->size);
		if (rv)
			goto bail;
		break;

	case SKS_CKM_AES_GCM:
		rv = tee_init_gcm_operation(session,
					    proc_params->data,
					    proc_params->size);
		if (rv)
			goto bail;
		break;

	default:
		TEE_Panic(TEE_ERROR_NOT_IMPLEMENTED);
	}

	session->proc_id = proc_params->id;
	rv = SKS_OK;

bail:
	if (rv)
		release_active_processing(session);

	TEE_Free(proc_params);

	return rv;
}

/*
 * ctrl = [session-handle]
 * in = data buffer
 * out = data buffer
 */
uint32_t entry_cipher_update(uintptr_t tee_session, TEE_Param *ctrl,
			     TEE_Param *in, TEE_Param *out, int decrypt)
{
	uint32_t rv;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	TEE_Result res;
	struct pkcs11_session *session;
	size_t in_size = in ? in->memref.size : 0;
	uint32_t out_size = out ? out->memref.size : 0;

	if (!ctrl)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	session = sks_handle2session(session_handle, tee_session);
	if (!session)
		return SKS_CKR_SESSION_HANDLE_INVALID;

	if (check_processing_state(session, decrypt ?
					    PKCS11_SESSION_DECRYPTING :
					    PKCS11_SESSION_ENCRYPTING))
		return SKS_CKR_OPERATION_NOT_INITIALIZED;

	switch (session->proc_id) {
	case SKS_CKM_AES_CCM:
	case SKS_CKM_AES_GCM:
		if (decrypt) {
			rv = tee_ae_decrypt_update(session, in ?
						   in->memref.buffer :
						   NULL, in_size);
			/* Keep decrypted data in secure memory until final */
			out_size = 0;
			break;
		}

		res = TEE_AEUpdate(session->tee_op_handle,
				   in ? in->memref.buffer : NULL, in_size,
				   out ? out->memref.buffer : NULL,
				   &out_size);

		rv = tee2sks_error(res);
		break;

	default:
		res = TEE_CipherUpdate(session->tee_op_handle,
					in ? in->memref.buffer : NULL,
					in_size,
					out ? out->memref.buffer : NULL,
					&out_size);

		rv = tee2sks_error(res);
		break;
	}

	if (!out && rv == SKS_SHORT_BUFFER)
		rv = SKS_BAD_PARAM;

	if (rv != SKS_OK && rv != SKS_SHORT_BUFFER)
		release_active_processing(session);
	else
		if (out)
			out->memref.size = out_size;

	return rv;
}

/*
 * ctrl = [session-handle]
 * in = none
 * out = data buffer
 */
uint32_t entry_cipher_final(uintptr_t tee_session, TEE_Param *ctrl,
			    TEE_Param *in, TEE_Param *out, int decrypt)
{
	uint32_t rv;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	TEE_Result res;
	struct pkcs11_session *session;
	size_t in_size = in ? in->memref.size : 0;
	uint32_t out_size = out ? out->memref.size : 0;

	/* May or may not provide input and/or output data */
	if (!ctrl)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	session = sks_handle2session(session_handle, tee_session);
	if (!session)
		return SKS_CKR_SESSION_HANDLE_INVALID;

	if (check_processing_state(session, decrypt ?
					    PKCS11_SESSION_DECRYPTING :
					    PKCS11_SESSION_ENCRYPTING))
		return SKS_CKR_OPERATION_NOT_INITIALIZED;

	switch (session->proc_id) {
	case SKS_CKM_AES_CCM:
	case SKS_CKM_AES_GCM:
		if (in_size) {
			/*
			 * Pkcs11 EncryptFinal and DecryptFinal to do provide
			 * input data reference, only an output buffer which
			 * is mandatory to produce the tag (encryption) or
			 * reveale the output data (decryption).
			 */
			rv = SKS_BAD_PARAM;
			break;
		}

		if (decrypt)
			rv = tee_ae_decrypt_final(session, out ?
						  out->memref.buffer : NULL,
						  &out_size);
		else
			rv = tee_ae_encrypt_final(session, out ?
						  out->memref.buffer : NULL,
						  &out_size);
		break;

	default:
		res = TEE_CipherDoFinal(session->tee_op_handle,
					in ? in->memref.buffer : NULL,
					in_size,
					out ? out->memref.buffer : NULL,
					&out_size);

		rv = tee2sks_error(res);
		break;
	}

	if (!out && rv == SKS_SHORT_BUFFER)
		rv = SKS_BAD_PARAM;

	if (out && (rv == SKS_OK || rv == SKS_SHORT_BUFFER))
		out->memref.size = out_size;

	/* Only a short buffer error can leave the operation active */
	if (rv != SKS_SHORT_BUFFER)
		release_active_processing(session);

	return rv;
}

static uint32_t generate_random_key_value(struct sks_attrs_head **head)
{
	uint32_t rv;
	void *data;
	size_t data_size;
	uint32_t value_len;
	void *value;

	if (!*head)
		return SKS_CKR_TEMPLATE_INCONSISTENT;

	rv = get_attribute_ptr(*head, SKS_CKA_VALUE_LEN, &data, &data_size);
	if (rv || data_size != sizeof(uint32_t)) {
		DMSG("%s", rv ? "No attribute value_len found" :
			"Invalid size for attribute VALUE_LEN");
		return SKS_CKR_ATTRIBUTE_VALUE_INVALID;
	}

	TEE_MemMove(&value_len, data, data_size);
	value = TEE_Malloc(value_len, TEE_USER_MEM_HINT_NO_FILL_ZERO);
	if (!value)
		return SKS_MEMORY;

	TEE_GenerateRandom(value, value_len);

	rv = add_attribute(head, SKS_CKA_VALUE, value, value_len);

	/* TODO: scratch content of heap memory where key was stored? */
	TEE_Free(value);

	return rv;
}

uint32_t entry_generate_object(uintptr_t tee_session,
			       TEE_Param *ctrl, TEE_Param *in, TEE_Param *out)
{
	uint32_t rv;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	struct pkcs11_session *session;
	struct sks_attribute_head *proc_params = NULL;
	struct sks_attrs_head *head = NULL;
	struct sks_object_head *template = NULL;
	size_t template_size;
	uint32_t obj_handle;

	if (!ctrl || in || !out)
		return SKS_BAD_PARAM;

	if (out->memref.size < sizeof(uint32_t)) {
		out->memref.size = sizeof(uint32_t);
		return SKS_SHORT_BUFFER;
	}

	if ((uintptr_t)out->memref.buffer & 0x3UL)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	rv = serialargs_alloc_get_one_attribute(&ctrlargs, &proc_params);
	if (rv)
		return rv;

	rv = serialargs_alloc_get_attributes(&ctrlargs, &template);
	if (rv)
		goto bail;

	template_size = sizeof(*template) + template->attrs_size;

	/*
	 * Check arguments
	 */

	session = sks_handle2session(session_handle, tee_session);
	if (!session) {
		rv = SKS_CKR_SESSION_HANDLE_INVALID;
		goto bail;
	}

	if (check_processing_state(session, PKCS11_SESSION_READY)) {
		rv = SKS_CKR_OPERATION_ACTIVE;
		goto bail;
	}

	/*
	 * Prepare a clean initial state for the requested object attributes.
	 * Free temorary template once done.
	 */
	rv = create_attributes_from_template(&head, template, template_size,
					     NULL, SKS_FUNCTION_GENERATE);
	if (rv)
		goto bail;

	TEE_Free(template);
	template = NULL;

	/*
	 * Check created object against processing and token state.
	 */
	rv = check_created_attrs_against_processing(proc_params->id, head);
	if (rv)
		goto bail;

	rv = check_created_attrs_against_token(session, head);
	if (rv)
		goto bail;

	/*
	 * Execute target processing and add value as attribute SKS_CKA_VALUE.
	 * Symm key generation: depens on target processing to be used.
	 */
	switch (proc_params->id) {
	case SKS_CKM_GENERIC_SECRET_KEY_GEN:
	case SKS_CKM_AES_KEY_GEN:
		/* Generate random of size specified by attribute VALUE_LEN */
		rv = generate_random_key_value(&head);
		if (rv)
			goto bail;
		break;

	default:
		rv = SKS_CKR_MECHANISM_INVALID;
		goto bail;
	}

	TEE_Free(proc_params);
	proc_params = NULL;

	/*
	 * Object is ready, register it and return a handle.
	 */
	rv = create_object(session, head, &obj_handle);
	if (rv)
		goto bail;

	/*
	 * Now obj_handle (through the related struct sks_object instance)
	 * owns the serialized buffer that holds the object attributes.
	 * We reset attrs->buffer to NULL as serializer object is no more
	 * the attributes buffer owner.
	 */
	head = NULL;

	TEE_MemMove(out->memref.buffer, &obj_handle, sizeof(uint32_t));
	out->memref.size = sizeof(uint32_t);

bail:
	TEE_Free(proc_params);
	TEE_Free(template);
	TEE_Free(head);

	return rv;
}

/*
 * ctrl = [session-handle][key-handle][mechanism-parameters]
 * in = none
 * out = none
 */
uint32_t entry_signverify_init(uintptr_t tee_session, TEE_Param *ctrl,
				TEE_Param *in, TEE_Param *out, int sign)
{
	uint32_t rv;
	TEE_Result res;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	struct pkcs11_session *session = NULL;
	struct sks_attribute_head *proc_params = NULL;
	uint32_t key_handle;
	struct sks_object *obj;

	if (!ctrl || in || out)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	rv = serialargs_get(&ctrlargs, &key_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	rv = serialargs_alloc_get_one_attribute(&ctrlargs, &proc_params);
	if (rv)
		return rv;

	/*
	 * Check arguments
	 */

	session = sks_handle2session(session_handle, tee_session);
	if (!session) {
		rv = SKS_CKR_SESSION_HANDLE_INVALID;
		goto bail;
	}

	if (check_processing_state(session, PKCS11_SESSION_READY)) {
		rv = SKS_CKR_OPERATION_ACTIVE;
		goto bail;
	}

	if (set_processing_state(session, sign ? PKCS11_SESSION_SIGNING :
						 PKCS11_SESSION_VERIFYING)) {
		rv = SKS_CKR_OPERATION_ACTIVE;
		goto bail;
	}

	obj = sks_handle2object(key_handle, session);
	if (!obj) {
		rv = SKS_CKR_KEY_HANDLE_INVALID;
		goto bail;
	}

	/*
	 * Check created object against processing and token state.
	 */
	rv = check_parent_attrs_against_processing(proc_params->id, sign ?
						   SKS_FUNCTION_SIGN :
						   SKS_FUNCTION_VERIFY,
						   obj->attributes);
	if (rv)
		goto bail;

	rv = check_parent_attrs_against_token(session, obj->attributes);
	if (rv)
		goto bail;

	/*
	 * Allocate a TEE operation for the target processing and
	 * fill it with the expected operation parameters.
	 */
	rv = tee_operarion_params(session, proc_params, obj, sign ?
				  SKS_FUNCTION_SIGN : SKS_FUNCTION_VERIFY);
	if (rv)
		goto bail;

	/*
	 * Execute target processing and add value as attribute SKS_CKA_VALUE.
	 * Symm key generation: depens on target processing to be used.
	 */
	switch (proc_params->id) {
	case SKS_CKM_AES_CMAC:
	case SKS_CKM_AES_CMAC_GENERAL:
	case SKS_CKM_MD5_HMAC:
	case SKS_CKM_SHA_1_HMAC:
	case SKS_CKM_SHA224_HMAC:
	case SKS_CKM_SHA256_HMAC:
	case SKS_CKM_SHA384_HMAC:
	case SKS_CKM_SHA512_HMAC:
	case SKS_CKM_AES_XCBC_MAC:
		rv = load_key(obj);
		if (rv)
			goto bail;

		break;

	default:
		rv = SKS_CKR_MECHANISM_INVALID;
		goto bail;
	}

	res = TEE_SetOperationKey(session->tee_op_handle, obj->key_handle);
	if (res) {
		EMSG("TEE_SetOperationKey failed %x", res);
		rv = tee2sks_error(res);
		goto bail;
	}

	/*
	 * Specifc cipher initialization if any
	 */
	switch (proc_params->id) {
	case SKS_CKM_AES_CMAC_GENERAL:
	case SKS_CKM_AES_CMAC:
	case SKS_CKM_MD5_HMAC:
	case SKS_CKM_SHA_1_HMAC:
	case SKS_CKM_SHA224_HMAC:
	case SKS_CKM_SHA256_HMAC:
	case SKS_CKM_SHA384_HMAC:
	case SKS_CKM_SHA512_HMAC:
	case SKS_CKM_AES_XCBC_MAC:
		// TODO: get the desired output size
		TEE_MACInit(session->tee_op_handle, NULL, 0);
		break;

	default:
		rv = SKS_CKR_MECHANISM_INVALID;
		goto bail;
	}

	session->proc_id = proc_params->id;
	rv = SKS_OK;

bail:
	if (rv && session)
		release_active_processing(session);

	TEE_Free(proc_params);

	return rv;
}

/*
 * ctrl = [session-handle]
 * in = input data
 * out = none
 */
uint32_t entry_signverify_update(uintptr_t tee_session, TEE_Param *ctrl,
				 TEE_Param *in, TEE_Param *out, int sign)
{
	struct serialargs ctrlargs;
	uint32_t session_handle;
	struct pkcs11_session *session;
	uint32_t rv;

	/* May or may not provide input and/or output data */
	if (!ctrl)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	session = sks_handle2session(session_handle, tee_session);
	if (!session)
		return SKS_CKR_SESSION_HANDLE_INVALID;

	if (check_processing_state(session, sign ? PKCS11_SESSION_SIGNING :
						 PKCS11_SESSION_VERIFYING))
		return SKS_CKR_OPERATION_NOT_INITIALIZED;

	if (!in || out) {
		rv = SKS_BAD_PARAM;
		goto bail;
	}

	switch (session->proc_id) {
	case SKS_CKM_AES_CMAC_GENERAL:
	case SKS_CKM_AES_CMAC:
	case SKS_CKM_MD5_HMAC:
	case SKS_CKM_SHA_1_HMAC:
	case SKS_CKM_SHA224_HMAC:
	case SKS_CKM_SHA256_HMAC:
	case SKS_CKM_SHA384_HMAC:
	case SKS_CKM_SHA512_HMAC:
	case SKS_CKM_AES_XCBC_MAC:
		TEE_MACUpdate(session->tee_op_handle,
				in->memref.buffer, in->memref.size);
		break;

	default:
		rv = SKS_CKR_MECHANISM_INVALID;
		goto bail;
	}

	rv = SKS_OK;

bail:
	if (rv)
		release_active_processing(session);

	return rv;
}

/*
 * ctrl = [session-handle]
 * in = none
 * out = data buffer
 */
uint32_t entry_signverify_final(uintptr_t tee_session, TEE_Param *ctrl,
				TEE_Param *in, TEE_Param *out, int sign)
{
	TEE_Result res;
	uint32_t rv;
	struct serialargs ctrlargs;
	uint32_t session_handle;
	struct pkcs11_session *session;
	uint32_t out_size = out ? out->memref.size : 0;

	if (!ctrl)
		return SKS_BAD_PARAM;

	serialargs_init(&ctrlargs, ctrl->memref.buffer, ctrl->memref.size);

	rv = serialargs_get(&ctrlargs, &session_handle, sizeof(uint32_t));
	if (rv)
		return rv;

	session = sks_handle2session(session_handle, tee_session);
	if (!session)
		return SKS_CKR_SESSION_HANDLE_INVALID;

	if (check_processing_state(session, sign ? PKCS11_SESSION_SIGNING :
						 PKCS11_SESSION_VERIFYING))
		return SKS_CKR_OPERATION_NOT_INITIALIZED;

	if (in || !out) {
		rv = SKS_BAD_PARAM;
		goto bail;
	}

	switch (session->proc_id) {
	case SKS_CKM_AES_CMAC_GENERAL:
	case SKS_CKM_AES_CMAC:
	case SKS_CKM_MD5_HMAC:
	case SKS_CKM_SHA_1_HMAC:
	case SKS_CKM_SHA224_HMAC:
	case SKS_CKM_SHA256_HMAC:
	case SKS_CKM_SHA384_HMAC:
	case SKS_CKM_SHA512_HMAC:
	case SKS_CKM_AES_XCBC_MAC:
		if (sign)
			res = TEE_MACComputeFinal(session->tee_op_handle,
						  NULL, 0, out->memref.buffer,
						  &out_size);
		else
			res = TEE_MACCompareFinal(session->tee_op_handle,
						  NULL, 0, out->memref.buffer,
						  out_size);

		rv = tee2sks_error(res);
		break;

	default:
		rv = SKS_CKR_MECHANISM_INVALID;
		goto bail;
	}

bail:
	if (sign && (rv == SKS_OK || rv == SKS_SHORT_BUFFER))
		out->memref.size = out_size;

	if (rv != SKS_SHORT_BUFFER)
		release_active_processing(session);

	return rv;
}
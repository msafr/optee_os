/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2017-2018, Linaro Limited
 */

#ifndef __SKS_OBJECT_H__
#define __SKS_OBJECT_H__

#include <sks_internal_abi.h>
#include <sys/queue.h>
#include <tee_internal_api.h>

struct pkcs11_session;

struct sks_object {
	LIST_ENTRY(sks_object) link;
	/* pointer to the serialized object attributes */
	void *attributes;
	TEE_ObjectHandle key_handle;
	/* These are for persistent/token objects (TODO: move to attributes) */
	TEE_UUID *uuid;
	TEE_ObjectHandle attribs_hdl;
};

LIST_HEAD(object_list, sks_object);

struct sks_object *sks_handle2object(uint32_t client_handle,
				     struct pkcs11_session *session);

uint32_t sks_object2handle(struct sks_object *obj,
			   struct pkcs11_session *session);

struct sks_object *create_token_object_instance(struct sks_attrs_head *head,
						TEE_UUID *uuid);


/*
 * create_object - create an SKS object from its attributes and value
 *
 * @session - session requesting object creation
 * @attributes - reference to serialized attributes
 * @handle - generated handle for the created object
 */
uint32_t create_object(void *session, struct sks_attrs_head *attributes,
			uint32_t *handle);

/*
 * destroy_object - destroy an SKS object
 *
 * @session - session requesting object destruction
 * @object - reference to the sks object
 * @session_object_only - true is only session object shall be destroyed
 */
uint32_t destroy_object(struct pkcs11_session *session,
			struct sks_object *object,
			bool session_object_only);

/*
 * Entry function called from the SKS command parser
 */
uint32_t entry_destroy_object(uintptr_t teesess, TEE_Param *ctrl,
			      TEE_Param *in, TEE_Param *out);

uint32_t entry_find_objects_init(uintptr_t teesess, TEE_Param *ctrl,
				 TEE_Param *in, TEE_Param *out);

uint32_t entry_find_objects(uintptr_t teesess, TEE_Param *ctrl,
			    TEE_Param *in, TEE_Param *out);

uint32_t entry_find_objects_final(uintptr_t teesess, TEE_Param *ctrl,
				  TEE_Param *in, TEE_Param *out);

void release_session_find_obj_context(struct pkcs11_session *session);

#endif /*__SKS_OBJECT_H__*/
/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_eap_sim.c
 * @brief Implements the SIM part of EAP-SIM
 *
 * The development of the EAP/SIM support was funded by Internet Foundation
 * Austria (http://www.nic.at/ipa).
 *
 * @copyright 2003  Michael Richardson <mcr@sandelman.ottawa.on.ca>
 * @copyright 2003-2016  The FreeRADIUS server project
 * @copyright 2016 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
RCSID("$Id$")

#include "../../eap.h"
#include "eap_types.h"
#include "eap_sim.h"
#include "sim_proto.h"

#include <freeradius-devel/rad_assert.h>

static fr_dict_attr_t const *dict_sim_root;

/*
 *	build a reply to be sent.
 */
static int eap_sim_compose(eap_session_t *eap_session)
{
	/* we will set the ID on requests, since we have to HMAC it */
	eap_session->this_round->set_request_id = true;

	return fr_sim_encode(eap_session->request, dict_sim_root,
			     eap_session->request->reply->vps, eap_session->this_round->request);
}

static int eap_sim_send_state(eap_session_t *eap_session)
{
	VALUE_PAIR		**vps, *newvp;
	uint16_t		words[3];
	eap_sim_session_t	*eap_sim_session;
	RADIUS_PACKET		*packet;
	uint8_t			*p;

	rad_assert(eap_session->request != NULL);
	rad_assert(eap_session->request->reply);

	eap_sim_session = talloc_get_type_abort(eap_session->opaque, eap_sim_session_t);

	/* these are the outgoing attributes */
	packet = eap_session->request->reply;
	vps = &packet->vps;
	rad_assert(vps != NULL);


	/*
	 *	Add appropriate TLVs for the EAP things we wish to send.
	 */

	/* the version list. We support only version 1. */
	words[0] = htons(sizeof(words[1]));
	words[1] = htons(EAP_SIM_VERSION);
	words[2] = 0;

	newvp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_VERSION_LIST);
	fr_pair_value_memcpy(newvp, (uint8_t const *) words, sizeof(words));

	fr_pair_add(vps, newvp);

	/* set the EAP_ID - new value */
	newvp = fr_pair_afrom_child_num(packet, fr_dict_root(fr_dict_internal), PW_EAP_ID);
	newvp->vp_integer = eap_sim_session->sim_id++;
	fr_pair_replace(vps, newvp);

	/* record it in the ess */
	eap_sim_session->keys.version_list_len = 2;
	memcpy(eap_sim_session->keys.version_list, words + 1, eap_sim_session->keys.version_list_len);

	/* the ANY_ID attribute. We do not support re-auth or pseudonym */
	MEM(newvp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_FULLAUTH_ID_REQ));
	MEM(p = talloc_array(newvp, uint8_t, 2));
	p[0] = 0;
	p[1] = 1;
	fr_pair_value_memsteal(newvp, p);
	fr_pair_add(vps, newvp);

	/* the SUBTYPE, set to start. */
	newvp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_SUBTYPE);
	newvp->vp_integer = EAP_SIM_START;
	fr_pair_replace(vps, newvp);

	return 1;
}

/** Send the challenge itself
 *
 * Challenges will come from one of three places eventually:
 *
 * 1  from attributes like PW_EAP_SIM_RANDx
 *	    (these might be retrieved from a database)
 *
 * 2  from internally implemented SIM authenticators
 *	    (a simple one based upon XOR will be provided)
 *
 * 3  from some kind of SS7 interface.
 *
 * For now, they only come from attributes.
 * It might be that the best way to do 2/3 will be with a different
 * module to generate/calculate things.
 *
 */
static int eap_sim_send_challenge(eap_session_t *eap_session)
{
	eap_sim_session_t	*eap_sim_session;
	VALUE_PAIR		**from_client, **to_client, *vp;
	RADIUS_PACKET		*packet;
	uint8_t			*p, *rand;

	eap_sim_session = talloc_get_type_abort(eap_session->opaque, eap_sim_session_t);
	rad_assert(eap_session->request != NULL);
	rad_assert(eap_session->request->reply);

	/*
	 *	Invps is the data from the client but this is for non-protocol data here.
	 *	We should already have consumed any client originated data.
	 */
	from_client = &eap_session->request->packet->vps;

	/*
	 *	Outvps is the data to the client
	 */
	packet = eap_session->request->reply;
	to_client = &packet->vps;

	/*
	 *	Okay, we got the challenges! Put them into an attribute.
	 */
	MEM(vp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_RAND));
	MEM(p = rand = talloc_array(vp, uint8_t, 2 + (EAP_SIM_RAND_SIZE * 3)));
	memset(p, 0, 2); /* clear reserved bytes */
	p += 2;
	memcpy(p, eap_sim_session->keys.vector[0].rand, EAP_SIM_RAND_SIZE);
	p += EAP_SIM_RAND_SIZE;
	memcpy(p, eap_sim_session->keys.vector[1].rand, EAP_SIM_RAND_SIZE);
	p += EAP_SIM_RAND_SIZE;
	memcpy(p, eap_sim_session->keys.vector[2].rand, EAP_SIM_RAND_SIZE);
	fr_pair_value_memsteal(vp, rand);
	fr_pair_add(to_client, vp);

	/*
	 *	Set the EAP_ID - new value
	 */
	vp = fr_pair_afrom_child_num(packet, fr_dict_root(fr_dict_internal), PW_EAP_ID);
	vp->vp_integer = eap_sim_session->sim_id++;
	fr_pair_replace(to_client, vp);

	/*
	 *	Use the SIM identity, if available
	 */
	vp = fr_pair_find_by_child_num(*from_client, dict_sim_root, PW_EAP_SIM_IDENTITY, TAG_ANY);
	if (vp) {
		MEM(eap_sim_session->keys.identity = (uint8_t *)talloc_bstrndup(eap_sim_session,
										vp->vp_strvalue, vp->vp_length));
		eap_sim_session->keys.identity_len = vp->vp_length;
	/*
	 *	Make a copy of the identity
	 */
	} else {
		if (eap_sim_session->keys.identity) talloc_free(eap_sim_session->keys.identity);

		eap_sim_session->keys.identity_len = strlen(eap_session->identity);

		MEM(eap_sim_session->keys.identity = talloc_array(eap_sim_session, uint8_t,
								  eap_sim_session->keys.identity_len));
		memcpy(eap_sim_session->keys.identity, eap_session->identity,
		       eap_sim_session->keys.identity_len);
	}

	/*
	 *	All set, calculate keys!
	 */
	fr_sim_crypto_keys_derive(&eap_sim_session->keys);

#ifdef EAP_SIM_DEBUG_PRF
	fr_sim_crypto_keys_log(&eap_sim_session->keys);
#endif

	/*
	 *	Need to include an AT_MAC attribute so that it will get
	 *	calculated. The NONCE_MT and the MAC are both 16 bytes, so
	 *	We store the NONCE_MT in the MAC for the encoder, which
	 *	will pull it out before it does the operation.
	 */
	vp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_MAC);
	fr_pair_value_memcpy(vp, eap_sim_session->keys.nonce_mt, 16);
	fr_pair_replace(to_client, vp);

	vp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_KEY);
	fr_pair_value_memcpy(vp, eap_sim_session->keys.k_aut, 16);
	fr_pair_replace(to_client, vp);

	/* the SUBTYPE, set to challenge. */
	vp = fr_pair_afrom_child_num(packet, dict_sim_root, PW_EAP_SIM_SUBTYPE);
	vp->vp_integer = EAP_SIM_CHALLENGE;
	fr_pair_replace(to_client, vp);

	return 1;
}

#ifndef EAPTLS_MPPE_KEY_LEN
#define EAPTLS_MPPE_KEY_LEN     32
#endif

/** Send a success message
 *
 * The only work to be done is the add the appropriate SEND/RECV
 * radius attributes derived from the MSK.
 */
static int eap_sim_send_success(eap_session_t *eap_session)
{
	uint8_t			*p;
	eap_sim_session_t	*eap_sim_session;
	VALUE_PAIR		*vp;
	RADIUS_PACKET		*packet;

	/* to_client is the data to the client. */
	packet = eap_session->request->reply;
	eap_sim_session = talloc_get_type_abort(eap_session->opaque, eap_sim_session_t);

	/* set the EAP_ID - new value */
	vp = fr_pair_afrom_child_num(packet, fr_dict_root(fr_dict_internal), PW_EAP_ID);
	vp->vp_integer = eap_sim_session->sim_id++;
	fr_pair_replace(&eap_session->request->reply->vps, vp);

	p = eap_sim_session->keys.msk;
	eap_add_reply(eap_session->request, "MS-MPPE-Recv-Key", p, EAPTLS_MPPE_KEY_LEN);
	p += EAPTLS_MPPE_KEY_LEN;
	eap_add_reply(eap_session->request, "MS-MPPE-Send-Key", p, EAPTLS_MPPE_KEY_LEN);

	return 1;
}

/** Run the server state machine
 *
 */
static void eap_sim_state_enter(eap_session_t *eap_session,
				eap_sim_session_t *eap_sim_session,
				eap_sim_server_state_t new_state)
{
	switch (new_state) {
	/*
	 * 	Send the EAP-SIM Start message, listing the versions that we support.
	 */
	case EAP_SIM_SERVER_START:
		eap_sim_send_state(eap_session);
		break;
	/*
	 *	Send the EAP-SIM Challenge message.
	 */
	case EAP_SIM_SERVER_CHALLENGE:
		eap_sim_send_challenge(eap_session);
		break;

	/*
	 * 	Send the EAP Success message
	 */
	case EAP_SIM_SERVER_SUCCESS:
		eap_sim_send_success(eap_session);
		eap_session->this_round->request->code = PW_EAP_SUCCESS;
		break;
	/*
	 *	Nothing to do for this transition.
	 */
	default:
		break;
	}

	eap_sim_session->state = new_state;

	/* build the target packet */
	eap_sim_compose(eap_session);
}

static int CC_HINT(nonnull) mod_process(void *instance, eap_session_t *eap_session);

/*
 *	Initiate the EAP-SIM session by starting the state machine
 *      and initiating the state.
 */
static int mod_session_init(UNUSED void *instance, eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	eap_sim_session_t	*eap_sim_session;
	time_t			n;
	eap_sim_vector_src_t	src = EAP_SIM_VECTOR_SRC_AUTO;

	MEM(eap_sim_session = talloc_zero(eap_session, eap_sim_session_t));

	eap_session->opaque = eap_sim_session;

	/*
	 *	Save the keying material, because it could change on a subsequent retrieval.
	 */
	RDEBUG2("New EAP-SIM session.  Acquiring SIM vectors");
	if ((eap_sim_vector_from_attrs(eap_session, request->control, 0, eap_sim_session, &src) != 0) ||
	    (eap_sim_vector_from_attrs(eap_session, request->control, 1, eap_sim_session, &src) < 0) ||
	    (eap_sim_vector_from_attrs(eap_session, request->control, 2, eap_sim_session, &src) < 0)) {
	    	REDEBUG("Failed retrieving SIM vectors");
		return 0;
	}

	/*
	 *	This value doesn't have be strong, but it is good if it is different now and then.
	 */
	time(&n);
	eap_sim_session->sim_id = (n & 0xff);

	eap_sim_state_enter(eap_session, eap_sim_session, EAP_SIM_SERVER_START);

	eap_session->process = mod_process;

	return 1;
}

/** Process an EAP-Sim/Response/Start
 *
 * Verify that client chose a version, and provided a NONCE_MT,
 * and if so, then change states to challenge, and send the new
 * challenge, else, resend the Request/Start.
 */
static int process_eap_sim_start(eap_session_t *eap_session, VALUE_PAIR *vps)
{
	REQUEST			*request = eap_session->request;
	VALUE_PAIR		*nonce_vp, *selected_version_vp;
	eap_sim_session_t	*eap_sim_session;
	uint16_t		eap_sim_version;

	eap_sim_session = talloc_get_type_abort(eap_session->opaque, eap_sim_session_t);

	nonce_vp = fr_pair_find_by_child_num(vps, dict_sim_root, PW_EAP_SIM_NONCE_MT, TAG_ANY);
	selected_version_vp = fr_pair_find_by_child_num(vps, dict_sim_root, PW_EAP_SIM_SELECTED_VERSION, TAG_ANY);
	if (!nonce_vp || !selected_version_vp) {
		RDEBUG2("Client did not select a version and send a NONCE");
		eap_sim_state_enter(eap_session, eap_sim_session, EAP_SIM_SERVER_START);

		return 1;
	}

	eap_sim_version = selected_version_vp->vp_short;
	if (eap_sim_version != EAP_SIM_VERSION) {
		REDEBUG("EAP-SIM version %i is unknown", eap_sim_version);
		return 0;
	}

	/*
	 *	Record it for later keying
	 */
	eap_sim_version = htons(eap_sim_version);
	memcpy(eap_sim_session->keys.version_select, &eap_sim_version, sizeof(eap_sim_session->keys.version_select));

	/*
	 *	Double check the nonce size.
	 */
	if (nonce_vp->vp_length != 18) {
		REDEBUG("EAP-SIM nonce_mt must be 16 bytes (+2 bytes padding), not %zu", nonce_vp->vp_length);
		return 0;
	}
	memcpy(eap_sim_session->keys.nonce_mt, nonce_vp->vp_octets + 2, 16);

	/*
	 *	Everything looks good, change states
	 */
	eap_sim_state_enter(eap_session, eap_sim_session, EAP_SIM_SERVER_CHALLENGE);

	return 1;
}

/** Process an EAP-Sim/Response/Challenge
 *
 * Verify that MAC that we received matches what we would have
 * calculated from the packet with the SRESx appended.
 */
static int process_eap_sim_challenge(eap_session_t *eap_session, VALUE_PAIR *vps)
{
	REQUEST *request = eap_session->request;
	eap_sim_session_t *eap_sim_session = talloc_get_type_abort(eap_session->opaque, eap_sim_session_t);

	uint8_t srescat[EAP_SIM_SRES_SIZE * 3];
	uint8_t *p = srescat;

	uint8_t calcmac[EAP_SIM_CALC_MAC_SIZE];

	memcpy(p, eap_sim_session->keys.vector[0].sres, EAP_SIM_SRES_SIZE);
	p += EAP_SIM_SRES_SIZE;
	memcpy(p, eap_sim_session->keys.vector[1].sres, EAP_SIM_SRES_SIZE);
	p += EAP_SIM_SRES_SIZE;
	memcpy(p, eap_sim_session->keys.vector[2].sres, EAP_SIM_SRES_SIZE);

	/*
	 *	Verify the MAC, now that we have all the keys
	 */
	if (fr_sim_crypto_mac_verify(eap_session, dict_sim_root, vps,
				     eap_sim_session->keys.k_aut,
				     srescat, sizeof(srescat), calcmac)) {
		RDEBUG2("MAC check succeed");
	} else {
		int i, j;
		char macline[20*3];
		char *m = macline;

		for (i = 0, j = 0; i < EAP_SIM_CALC_MAC_SIZE; i++) {
			if (j == 4) {
				*m++ = '_';
				j=0;
			}
			j++;

			sprintf(m, "%02x", calcmac[i]);
			m = m + strlen(m);
		}
		REDEBUG("Calculated MAC (%s) did not match", macline);
		return 0;
	}

	/* everything looks good, change states */
	eap_sim_state_enter(eap_session, eap_sim_session, EAP_SIM_SERVER_SUCCESS);

	return 1;
}


/** Authenticate a previously sent challenge
 *
 */
static int mod_process(UNUSED void *arg, eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	eap_sim_session_t	*eap_sim_session = talloc_get_type_abort(eap_session->opaque, eap_sim_session_t);

	fr_sim_decode_ctx_t	ctx;
	VALUE_PAIR		*vp, *vps;
	vp_cursor_t		cursor;

	eap_sim_subtype_t	subtype;

	int			ret;

	memset(&ctx, 0, sizeof(ctx));

	/*
	 *	VPS is the data from the client
	 */
	vps = eap_session->request->packet->vps;

	fr_cursor_init(&cursor, &request->packet->vps);

	ctx.keys = &(eap_sim_session->keys);
	ret = fr_sim_decode(eap_session->request,
			    &cursor, dict_sim_root,
			    eap_session->this_round->response->type.data,
			    eap_session->this_round->response->type.length,
			    &ctx);
	if (ret < 0) return 0;

	vp = fr_cursor_next(&cursor);
	if (vp && RDEBUG_ENABLED2) {
		RDEBUG2("EAP-SIM decoded attributes");
		rdebug_pair_list(L_DBG_LVL_2, request, vp, NULL);
	}

	/*
	 *	See what kind of message we have gotten
	 */
	vp = fr_pair_find_by_child_num(vps, dict_sim_root, PW_EAP_SIM_SUBTYPE, TAG_ANY);
	if (!vp) {
		REDEBUG2("No subtype attribute was created, message dropped");
		return 0;
	}
	subtype = vp->vp_integer;

	/*
	 *	Client error supersedes anything else.
	 */
	if (subtype == EAP_SIM_CLIENT_ERROR) {
		REDEBUG("Client encountered an error");
		return 0;
	}

	switch (eap_sim_session->state) {
	case EAP_SIM_SERVER_START:
		switch (subtype) {
		/*
		 *	Pretty much anything else here is illegal, so we will retransmit the request.
		 */
		default:
			eap_sim_state_enter(eap_session, eap_sim_session, EAP_SIM_SERVER_START);
			return 1;
		/*
		 * 	A response to our EAP-Sim/Request/Start!
		 */
		case EAP_SIM_START:
			return process_eap_sim_start(eap_session, vps);
		}

	case EAP_SIM_SERVER_CHALLENGE:
		switch (subtype) {
		/*
		 *	Pretty much anything else here is illegal, so we will retransmit the request.
		 */
		default:
			eap_sim_state_enter(eap_session, eap_sim_session, EAP_SIM_SERVER_CHALLENGE);
			return 1;
		/*
		 *	A response to our EAP-Sim/Request/Challenge!
		 */
		case EAP_SIM_CHALLENGE:
			return process_eap_sim_challenge(eap_session, vps);
		}

	default:
		rad_assert(0);
	}

	return 1;
}

static int mod_load(void)
{
	dict_sim_root = fr_dict_attr_child_by_num(fr_dict_root(fr_dict_internal), PW_EAP_SIM_ROOT);
	if (!dict_sim_root) {
		ERROR("Missing EAP-SIM-Root attribute");
		return -1;
	}
	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
extern rlm_eap_submodule_t rlm_eap_sim;
rlm_eap_submodule_t rlm_eap_sim = {
	.name		= "eap_sim",
	.magic		= RLM_MODULE_INIT,
	.load		= mod_load,
	.session_init	= mod_session_init,	/* Initialise a new EAP session */
	.process	= mod_process,		/* Process next round of EAP method */
};

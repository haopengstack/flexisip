/*
 Flexisip, a flexible SIP proxy server with media capabilities.
 Copyright (C) 2010-2015  Belledonne Communications SARL, All rights reserved.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as
 published by the Free Software Foundation, either version 3 of the
 License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sofia-sip/msg_addr.h>
#include <sofia-sip/sip_extra.h>
#include <sofia-sip/sip_status.h>

#include "module-auth.hh"
#include "auth/flexisip-auth-module.hh"

using namespace std;
using namespace flexisip;

// ====================================================================================================================
//  Authentication class
// ====================================================================================================================

Authentication::Authentication(Agent *ag) : ModuleAuthenticationBase(ag) {}

Authentication::~Authentication() {
	if (mRequiredSubjectCheckSet){
		regfree(&mRequiredSubject);
	}
}

void Authentication::onDeclare(GenericStruct *mc) {
	ModuleAuthenticationBase::onDeclare(mc);
	ConfigItemDescriptor items[] = {
		{StringList, "trusted-hosts", "List of whitespace separated IP which will not be challenged.", ""},
		{String, "db-implementation",
			"Database backend implementation for digest authentication [odbc,soci,file].",
			"file"
		},
		{String, "datasource",
			"Odbc connection string to use for connecting to database. "
			"ex1: DSN=myodbc3; where 'myodbc3' is the datasource name. "
			"ex2: DRIVER={MySQL};SERVER=host;DATABASE=db;USER=user;PASSWORD=pass;OPTION=3; for a DSN-less connection. "
			"ex3: /etc/flexisip/passwd; for a file containing user credentials in clear-text, md5 or sha256. "
			"The file must start with 'version:1' as the first line, and then contains lines in the form of:\n"
			"user@domain clrtxt:clear-text-password md5:md5-password sha256:sha256-password ;\n"
			"For example: \n"
			"bellesip@sip.linphone.org clrtxt:secret ;\n"
			"bellesip@sip.linphone.org md5:97ffb1c6af18e5687bf26cdf35e45d30 ;\n"
			"bellesip@sip.linphone.org clrtxt:secret md5:97ffb1c6af18e5687bf26cdf35e45d30 sha256:d7580069de562f5c7fd932cc986472669122da91a0f72f30ef1b20ad6e4f61a3 ;",
			""
		},
		{Integer, "cache-expire", "Duration of the validity of the credentials added to the cache in seconds.", "1800"},
		{Boolean, "hashed-passwords",
			"True if retrieved passwords from the database are hashed. HA1=MD5(A1) = MD5(username:realm:pass).",
			"false"
		},
		{Boolean, "reject-wrong-client-certificates",
			"If set to true, the module will simply reject with 403 forbidden any request coming from client"
			" who presented a bad TLS certificate (regardless of reason: improper signature, unmatched subjects)."
			" Otherwise, the module will fallback to a digest authentication.\n"
			"This policy applies only for transports configured with 'required-peer-certificate=1' parameter; indeed"
			" no certificate is requested to the client otherwise.",
			"false"
		},
		{String, "tls-client-certificate-required-subject", "An optional regular expression matched against subjects "
			"of presented client certificates. If this regular expression evaluates to false, the request is rejected. "
			"The matched subjects are, in order: subjectAltNames.DNS, subjectAltNames.URI, subjectAltNames.IP and CN.",
			""
		},
		{Boolean, "new-auth-on-407", "When receiving a proxy authenticate challenge, generate a new challenge for "
			"this proxy.", "false"},
		{Boolean, "enable-test-accounts-creation",
			"Enable a feature useful for automatic tests, allowing a client to create a temporary account in the "
			"password database in memory."
			"This MUST not be used for production as it is a real security hole.",
			"false"
		},
		{StringList, "trusted-client-certificates", "List of whitespace separated username or username@domain CN "
			"which will trusted. If no domain is given it is computed.",
			""
		},
		{Boolean, "trust-domain-certificates",
			"If enabled, all requests which have their request URI containing a trusted domain will be accepted.",
			"false"
		},
		config_item_end
	};

	mc->addChildrenValues(items);
	mc->get<ConfigBoolean>("hashed-passwords")->setDeprecated(true);
	//we deprecate "trusted-client-certificates" because "tls-client-certificate-required-subject" can do more.
	mc->get<ConfigStringList>("trusted-client-certificates")->setDeprecated(true);

	// Call declareConfig for backends
	AuthDbBackend::declareConfig(mc);

	mCountAsyncRetrieve = mc->createStat("count-async-retrieve", "Number of asynchronous retrieves.");
	mCountSyncRetrieve = mc->createStat("count-sync-retrieve", "Number of synchronous retrieves.");
	mCountPassFound = mc->createStat("count-password-found", "Number of passwords found.");
	mCountPassNotFound = mc->createStat("count-password-not-found", "Number of passwords not found.");
}

void Authentication::onLoad(const GenericStruct *mc) {
	ModuleAuthenticationBase::onLoad(mc);

	loadTrustedHosts(*mc->get<ConfigStringList>("trusted-hosts"));
	mNewAuthOn407 = mc->get<ConfigBoolean>("new-auth-on-407")->read();
	mTrustedClientCertificates = mc->get<ConfigStringList>("trusted-client-certificates")->read();
	mTrustDomainCertificates = mc->get<ConfigBoolean>("trust-domain-certificates")->read();
	mTestAccountsEnabled = mc->get<ConfigBoolean>("enable-test-accounts-creation")->read();

	string requiredSubject = mc->get<ConfigString>("tls-client-certificate-required-subject")->read();
	if (!requiredSubject.empty()){
		int res = regcomp(&mRequiredSubject, requiredSubject.c_str(),  REG_EXTENDED|REG_NOSUB);
		if (res != 0) {
			string err_msg(128, '\0');
			regerror(res, &mRequiredSubject, &err_msg[0], err_msg.size());
			LOGF("Could not compile regex for 'tls-client-certificate-required-subject' '%s': %s",
				 requiredSubject.c_str(),
				 err_msg.c_str()
			);
		} else mRequiredSubjectCheckSet = true;
	}
	mRejectWrongClientCertificates = mc->get<ConfigBoolean>("reject-wrong-client-certificates")->read();
	AuthDbBackend::get(); // force instanciation of the AuthDbBackend NOW, to force errors to arrive now if any.
}

bool Authentication::handleTestAccountCreationRequests(const shared_ptr<RequestSipEvent> &ev) {
	sip_t *sip = ev->getSip();
	if (sip->sip_request->rq_method == sip_method_register) {
		sip_unknown_t *h = ModuleToolbox::getCustomHeaderByName(sip, "X-Create-Account");
		if (h && strcasecmp(h->un_value, "yes") == 0) {
			url_t *url = sip->sip_from->a_url;
			if (url) {
				sip_unknown_t *h2 = ModuleToolbox::getCustomHeaderByName(sip, "X-Phone-Alias");
				const char* phone_alias = h2 ? h2->un_value : NULL;
				phone_alias = phone_alias ? phone_alias : "";
				AuthDbBackend::get().createAccount(url->url_user, url->url_host, url->url_user, url->url_password,
													sip->sip_expires->ex_delta, phone_alias);

				ostringstream os;
				os << "Account created for " << url->url_user << '@' << url->url_host << " with password "
					<< url->url_password << " and expires " << sip->sip_expires->ex_delta;
				if (phone_alias) os << " with phone alias " << phone_alias;
				SLOGD << os.str();
				return true;
			}
		}
	}
	return false;
}

bool Authentication::isTrustedPeer(const shared_ptr<RequestSipEvent> &ev) {
	sip_t *sip = ev->getSip();

	// Check for trusted host
	sip_via_t *via = sip->sip_via;
	list<BinaryIp>::const_iterator trustedHostsIt = mTrustedHosts.begin();
	const char *printableReceivedHost = !empty(via->v_received) ? via->v_received : via->v_host;

	BinaryIp receivedHost(printableReceivedHost, true);

	for (; trustedHostsIt != mTrustedHosts.end(); ++trustedHostsIt) {
		if (receivedHost == *trustedHostsIt) {
			LOGD("Allowing message from trusted host %s", printableReceivedHost);
			return true;
		}
	}
	return false;
}

bool Authentication::tlsClientCertificatePostCheck(const shared_ptr<RequestSipEvent> &ev){
	if (mRequiredSubjectCheckSet){
		bool ret = ev->matchIncomingSubject(&mRequiredSubject);
		if (ret){
			SLOGD<<"TLS certificate postcheck successful.";
		}else{
			SLOGUE<<"TLS certificate postcheck failed.";
		}
		return ret;
	}
	return true;
}

/* This function returns
 * true: if the tls authentication is handled (either successful or rejected)
 * false: if we have to fallback to digest
 */
bool Authentication::handleTlsClientAuthentication(const shared_ptr<RequestSipEvent> &ev) {
	sip_t *sip = ev->getSip();
	shared_ptr<tport_t> inTport = ev->getIncomingTport();
	unsigned int policy = 0;

	tport_get_params(inTport.get(), TPTAG_TLS_VERIFY_POLICY_REF(policy), NULL);
	// Check TLS certificate
	if ((policy & TPTLS_VERIFY_INCOMING) && tport_is_server(inTport.get())){
		/* tls client certificate is required for this transport*/
		if (tport_is_verified(inTport.get())) {
			/*the certificate looks good, now match subjects*/
			const url_t *from = sip->sip_from->a_url;
			const char *fromDomain = from->url_host;
			const char *res = NULL;
			url_t searched_uri = URL_INIT_AS(sip);
			SofiaAutoHome home;
			char *searched;

			searched_uri.url_host = from->url_host;
			searched_uri.url_user = from->url_user;
			searched = url_as_string(home.home(), &searched_uri);

			if (ev->findIncomingSubject(searched)) {
				SLOGD << "Allowing message from matching TLS certificate";
				goto postcheck;
			} else if (sip->sip_request->rq_method != sip_method_register &&
				(res = findIncomingSubjectInTrusted(ev, fromDomain))) {
				SLOGD << "Found trusted TLS certificate " << res;
			goto postcheck;
				} else {
					/*case where the certificate would work for the entire domain*/
					searched_uri.url_user = NULL;
					searched = url_as_string(home.home(), &searched_uri);
					if (ev->findIncomingSubject(searched)) {
						SLOGD << "Found TLS certificate for entire domain";
						goto postcheck;
					}
				}

				if (sip->sip_request->rq_method != sip_method_register && mTrustDomainCertificates) {
					searched_uri.url_user = NULL;
					searched_uri.url_host = sip->sip_request->rq_url->url_host;
					searched = url_as_string(home.home(), &searched_uri);
					if (ev->findIncomingSubject(searched)) {
						SLOGD << "Found trusted TLS certificate for the request URI domain";
						goto postcheck;
					}
				}

				LOGE("Client is presenting a TLS certificate not matching its identity.");
				SLOGUE << "Registration failure for " << url_as_string(home.home(), from)
					<< ", TLS certificate doesn't match its identity";
				goto bad_certificate;

				postcheck:
				if (tlsClientCertificatePostCheck(ev)){
					/*all is good, return true*/
					return true;
				}else goto bad_certificate;
		}else goto bad_certificate;

		bad_certificate:
		if (mRejectWrongClientCertificates){
			ev->reply(403, "Bad tls client certificate", SIPTAG_SERVER_STR(getAgent()->getServerString()), TAG_END());
			return true; /*the request is responded, no further processing required*/
		}
		/*fallback to digest*/
		return false;
	}
	/*no client certificate requested, go to digest auth*/
	return false;
}

void Authentication::onResponse(shared_ptr<ResponseSipEvent> &ev) {
	if (!mNewAuthOn407) return; /*nop*/

	shared_ptr<OutgoingTransaction> transaction = dynamic_pointer_cast<OutgoingTransaction>(ev->getOutgoingAgent());
	if (transaction == NULL) return;

	shared_ptr<string> proxyRealm = transaction->getProperty<string>("this_proxy_realm");
	if (proxyRealm == NULL) return;

	sip_t *sip = ev->getMsgSip()->getSip();
	if (sip->sip_status->st_status == 407 && sip->sip_proxy_authenticate) {
		auto *as = new FlexisipAuthStatus(nullptr);
		as->realm(proxyRealm.get()->c_str());
		as->userUri(sip->sip_from->a_url);
		AuthModule *am = findAuthModule(as->realm());
		FlexisipAuthModule *fam = dynamic_cast<FlexisipAuthModule *>(am);
		if (fam) {
			fam->challenge(*as, &mProxyChallenger);
			fam->nonceStore().insert(as->response());
			msg_header_insert(ev->getMsgSip()->getMsg(), (msg_pub_t *)sip, (msg_header_t *)as->response());
		} else {
			LOGD("Authentication module for %s not found", as->realm());
		}
	} else {
		LOGD("not handled newauthon401");
	}
}


void Authentication::onIdle() {
	for (auto &it : mAuthModules) {
		AuthModule *am = it.second.get();
		FlexisipAuthModule *fam = dynamic_cast<FlexisipAuthModule *>(am);
		fam->nonceStore().cleanExpired();
	}
}

bool Authentication::doOnConfigStateChanged(const ConfigValue &conf, ConfigState state) {
	if (conf.getName() == "trusted-hosts" && state == ConfigState::Commited) {
		loadTrustedHosts((const ConfigStringList &)conf);
		LOGD("Trusted hosts updated");
		return true;
	} else {
		return Module::doOnConfigStateChanged(conf, state);
	}
}

// ================================================================================================================= //
// Private methods                                                                                                   //
// ================================================================================================================= //

FlexisipAuthModuleBase *Authentication::createAuthModule(const std::string &domain, const std::string &algorithm) {
	FlexisipAuthModule *authModule = new FlexisipAuthModule(getAgent()->getRoot(), domain, mAlgorithms.front());
	authModule->setOnPasswordFetchResultCb([this](bool passFound){passFound ? mCountPassFound++ : mCountPassNotFound++;});
	SLOGI << "Found auth domain: " << domain;
	return authModule;
}

FlexisipAuthModuleBase *Authentication::createAuthModule(const std::string &domain, const std::string &algorithm, int nonceExpire) {
	FlexisipAuthModule *authModule = new FlexisipAuthModule(getAgent()->getRoot(), domain, mAlgorithms.front(), nonceExpire);
	authModule->setOnPasswordFetchResultCb([this](bool passFound){passFound ? mCountPassFound++ : mCountPassNotFound++;});
	SLOGI << "Found auth domain: " << domain;
	return authModule;
}

void Authentication::validateRequest(const std::shared_ptr<RequestSipEvent> &request) {
	sip_t *sip = request->getMsgSip()->getSip();

	ModuleAuthenticationBase::validateRequest(request);

	// handle account creation request (test feature only)
	if (mTestAccountsEnabled && handleTestAccountCreationRequests(request)) {
		request->reply(
			200,
			"Test account created",
			SIPTAG_SERVER_STR(getAgent()->getServerString()),
			SIPTAG_CONTACT(sip->sip_contact),
			SIPTAG_EXPIRES_STR("0"),
			TAG_END()
		);
		throw StopRequestProcessing();
	}

	// Check trusted peer
	if (isTrustedPeer(request))
		throw StopRequestProcessing();
}

void Authentication::processAuthentication(const std::shared_ptr<RequestSipEvent> &request, FlexisipAuthModuleBase &am) {
	// check if TLS client certificate provides sufficent authentication for this request.
	if (handleTlsClientAuthentication(request))
		throw StopRequestProcessing();

	// Create incoming transaction if not already exists
	// Necessary in qop=auth to prevent nonce count chaos
	// with retransmissions.
	request->createIncomingTransaction();

	ModuleAuthenticationBase::processAuthentication(request, am);
}

const char *Authentication::findIncomingSubjectInTrusted(const shared_ptr<RequestSipEvent> &ev, const char *fromDomain) {
	if (mTrustedClientCertificates.empty())
		return NULL;
	list<string> toCheck;
	for (auto it = mTrustedClientCertificates.cbegin(); it != mTrustedClientCertificates.cend(); ++it) {
		if (it->find("@") != string::npos)
			toCheck.push_back(*it);
		else
			toCheck.push_back(*it + "@" + string(fromDomain));
	}
	const char *res = ev->findIncomingSubject(toCheck);
	return res;
}

void Authentication::loadTrustedHosts(const ConfigStringList &trustedHosts) {
	list<string> hosts = trustedHosts.read();
	transform(hosts.begin(), hosts.end(), back_inserter(mTrustedHosts), [](string host) {
		return BinaryIp(host.c_str());
	});

	const GenericStruct *clusterSection = GenericManager::get()->getRoot()->get<GenericStruct>("cluster");
	bool clusterEnabled = clusterSection->get<ConfigBoolean>("enabled")->read();
	if (clusterEnabled) {
		list<string> clusterNodes = clusterSection->get<ConfigStringList>("nodes")->read();
		for (list<string>::const_iterator node = clusterNodes.cbegin(); node != clusterNodes.cend(); node++) {
			BinaryIp nodeIp((*node).c_str());

			if (find(mTrustedHosts.cbegin(), mTrustedHosts.cend(), nodeIp) == mTrustedHosts.cend()) {
				mTrustedHosts.push_back(nodeIp);
			}
		}
	}

	const GenericStruct *presenceSection = GenericManager::get()->getRoot()->get<GenericStruct>("module::Presence");
	bool presenceServer = presenceSection->get<ConfigBoolean>("enabled")->read();
	if (presenceServer) {
		SofiaAutoHome home;
		string presenceServer = presenceSection->get<ConfigString>("presence-server")->read();
		sip_contact_t *contact = sip_contact_make(home.home(), presenceServer.c_str());
		url_t *url = contact ? contact->m_url : NULL;
		if (url && url->url_host) {
			BinaryIp host(url->url_host);

			if (find(mTrustedHosts.cbegin(), mTrustedHosts.cend(), host) == mTrustedHosts.cend()) {
				SLOGI << "Adding presence server '" << url->url_host << "' to trusted hosts";
				mTrustedHosts.push_back(host);
			}
		} else {
			SLOGW << "Could not parse presence server URL '" << presenceServer
				<< "', cannot be added to trusted hosts!";
		}
	}
}

ModuleInfo<Authentication> Authentication::sInfo(
	"Authentication",
	"The authentication module challenges and authenticates SIP requests using two possible methods:\n"
	" * if the request is received via a TLS transport and 'require-peer-certificate' is set in transport definition "
	"in [Global] section for this transport, then the From header of the request is matched with the CN claimed by "
	"the client certificate. The CN must contain sip:user@domain or alternate name with URI=sip:user@domain "
	"corresponding to the URI in the from header for the request to be accepted. Optionnaly, the property "
	"tls-client-certificate-required-subject may contain a regular expression for additional checks to execute on "
	"certificate subjects.\n"
	" * if no TLS client based authentication can be performed, or is failed, then a SIP digest authentication is "
	"performed. The password verification is made by querying a database or a password file on disk.",
	{ "NatHelper" },
	ModuleInfoBase::ModuleOid::Authentication
);

// ====================================================================================================================

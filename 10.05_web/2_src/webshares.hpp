/* webshares.hpp: the operator's OS account and the file shares

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.
*/

#ifndef _WEBSHARES_HPP_
#define _WEBSHARES_HPP_

#include <string>

// The service account that owns the image tree and runs the shares.
#define WEBSHARES_SERVICE_USER "qunilator"

// True when name may become the operator account. On refusal, false with the
// reason in *error: a name that is not a portable user name, one this board
// reserves, or one that already belongs to an account this service did not
// create.
bool webshares_name_acceptable(const std::string &name, std::string *error);

// Make name the account SMB, FTP and SFTP answer to, with password, retiring
// previous when it names a different operator account. An empty name puts the
// shares back on the service account alone. Root-only and best effort, so a
// development host and a board without the shares installed both do nothing.
void webshares_apply(const std::string &previous, const std::string &name,
		const std::string &password);

// Make key the ssh public key the operator's account answers to, replacing any
// it held. On refusal, false with the reason in *error: a key OpenSSH would not
// accept, or a board with no operator account yet.
bool webshares_set_ssh_key(const std::string &name, const std::string &key,
		std::string *error);

// True when the operator's account holds an ssh key.
bool webshares_has_ssh_key(const std::string &name);

#endif // _WEBSHARES_HPP_

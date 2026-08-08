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

// Make name the account the web interface, SMB, FTP, SFTP and ssh answer to,
// with password, retiring previous when it names a different operator account.
// Root-only and best effort, so a development host and an installation without
// the shares both do nothing.
void webshares_apply(const std::string &previous, const std::string &name,
		const std::string &password);

// The same, from hashes somebody else derived: a card prepared by a tool
// carries the Linux account's crypt(3) hash and the file shares' NT hash, so
// the password itself never reaches the card. Same shaping of the account and
// the share configuration as above, and the same best-effort silence off a
// board.
void webshares_apply_hashed(const std::string &previous, const std::string &name,
		const std::string &unix_hash, const std::string &nt_hash);

// Take an account that exists already and make it the operator's, keeping its
// home, its files and its shell. This is the name that was a personal login
// before an operator was a thing; the interface refuses such a name, because
// taking over an account is a decision to make at the machine rather than in a
// browser. On refusal, false with the reason in *error.
bool webshares_adopt_account(const std::string &name, std::string *error);

// Whether an account of this name is on the machine at all, which is what tells
// adopting one from creating one. A card carrying a name nobody here has is
// asking for that account to be made; the same card in a machine that already
// has it is asking for that account to be taken over.
bool webshares_account_exists(const std::string &name);

// Make key the ssh public key the operator's account answers to, replacing any
// it held. On refusal, false with the reason in *error: a key OpenSSH would not
// accept, or a board with no operator account yet.
bool webshares_set_ssh_key(const std::string &name, const std::string &key,
		std::string *error);

// True when the operator's account holds an ssh key.
bool webshares_has_ssh_key(const std::string &name);

#endif // _WEBSHARES_HPP_

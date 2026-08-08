/* webshares.cpp: the operator's OS account and the file shares

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   SMB, FTP and SFTP all serve /var/lib/qunilator/images, and all three
   authenticate against the OS. The operator's web user name is therefore an OS
   account, created here beside the qunilator service account when the name is
   set and removed again when it changes.

   The service account keeps owning the tree and running the emulator, and takes
   no login: its password is locked here, so the operator's account is the one
   account a QUniLator answers to, over the web interface, the file shares and
   ssh alike. It has a home of its own under /home and a login shell, and every
   right it holds comes from a group - the image tree from qunilator, its
   primary group, and sudo from qunilator-admin, whose sudoers rule and group
   the package ships. Samba's "force user" keeps every file the share writes
   owned by qunilator, so a name change costs one account and no walk over the
   tree.

   The three share configurations name the qunilator *group*, which both
   accounts belong to, so a name change touches accounts rather than
   configuration. sshd confines an account that is in that group and not in
   qunilator-admin to an SFTP session in the image tree, which is what keeps a
   share-only account out of a shell. An installation predating this rewrites
   its files once, on the first name that is set, and checks its ssh
   configuration with sshd -t before asking sshd to read it.

   An ssh public key set through the interface is what makes the login usable
   from a workstation; the account holds nothing else that a share login does
   not already have.

   The base image is onboarded with a shared account whose password is the same
   on every SD card written from that image. Creating the operator's account is
   what makes that one unnecessary, so its password is locked at the same
   moment.

   Everything here is root-only and best effort. A development host has no
   useradd to run and no shares to serve, and does nothing.

   The password reaches useradd's siblings on stdin, never on a command line,
   and is never logged. No shell takes part in running any of them, so a name
   is an argument rather than something a shell could read.
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "weblog.hpp"
#include "webshares.hpp"

static const char *IMAGES_DIR = "/var/lib/qunilator/images";
static const char *OPERATOR_SHELL = "/bin/bash";
static const char *HOME_BASE = "/home";

// Membership carries the right to sudo: the package ships the group and the
// sudoers rule that names it, and the operator's account is put in it.
#define WEBSHARES_ADMIN_GROUP "qunilator-admin"

// Written into the account's GECOS field, and the only thing that makes an
// account ours: a name is refused when it belongs to an account without this
// mark, and only an account carrying it is ever removed.
static const char *OPERATOR_MARK = "QUniLator operator";

// The account the BeagleBone base image is onboarded with. Its password is the
// same on every SD card written from that image and is printed in that image's
// documentation, so it is a way in for anyone who can reach it. It is the way
// onto an installation nobody has set up yet, and it stops being needed the
// moment the operator has an account of their own; the password is locked then,
// leaving the account, its home and its groups where they are. "sudo usermod
// --unlock debian" puts it back.
static const char *ONBOARDING_ACCOUNT = "debian";

static const char *USERADD = "/usr/sbin/useradd";
static const char *USERDEL = "/usr/sbin/userdel";
static const char *USERMOD = "/usr/sbin/usermod";
static const char *GROUPADD = "/usr/sbin/groupadd";
static const char *CHPASSWD = "/usr/sbin/chpasswd";
static const char *SMBPASSWD = "/usr/bin/smbpasswd";
static const char *PDBEDIT = "/usr/bin/pdbedit";
static const char *SYSTEMCTL = "/usr/bin/systemctl";
static const char *SSHD = "/usr/sbin/sshd";

static const char *SHADOW_FILE = "/etc/shadow";
static const char *SMB_CONF = "/etc/samba/smb.conf";
static const char *FTP_USERLIST = "/etc/vsftpd.userlist";
static const char *SSHD_CONF_DIR = "/etc/ssh/sshd_config.d";

// Names a QUniLator keeps for itself, refused whether or not the account
// happens to exist here. Accounts that do exist are refused by the ownership
// check below, so this only has to cover the ones an installation might be
// missing.
static const char *RESERVED_NAMES[] = {
	"root", WEBSHARES_SERVICE_USER, "admin", "administrator", "adm", "daemon",
	"bin", "sys", "sync", "games", "man", "lp", "mail", "news", "uucp",
	"proxy", "backup", "list", "irc", "gnats", "nobody", "nogroup", "sshd",
	"ftp", "ftpuser", "anonymous", "www-data", "messagebus", "avahi",
	"sambashare", "systemd-network", "systemd-resolve", "systemd-timesync",
	"debian", "operator", "guest", nullptr
};

/*** running a program ***/

// Run argv[0] with argv and input on its stdin. Result is the exit status, or
// -1 when the program could not be run at all. execv takes the argument vector
// as it stands, so no shell parses any of it.
static int run_fed(const char *const argv[], const std::string &input) {
	int fds[2];
	if (pipe(fds) != 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		close(fds[1]);
		if (dup2(fds[0], STDIN_FILENO) < 0)
			_exit(127);
		close(fds[0]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execv(argv[0], (char *const *) argv);
		_exit(127);
	}
	close(fds[0]);
	// civetweb ignores SIGPIPE for the whole process, so a child that exits
	// before reading gives a short write rather than a signal
	size_t written = 0;
	while (written < input.size()) {
		ssize_t n = write(fds[1], input.data() + written, input.size() - written);
		if (n <= 0)
			break;
		written += (size_t) n;
	}
	close(fds[1]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run(const char *const argv[]) {
	return run_fed(argv, std::string());
}

static bool have(const char *path) {
	return access(path, X_OK) == 0;
}

// Ask a unit to read its configuration again. Missing units and missing
// systemd are both fine; an installation that serves no shares has nothing to
// reload.
static void reload_unit(const char *verb, const char *unit) {
	if (!have(SYSTEMCTL))
		return;
	const char *argv[] = { SYSTEMCTL, verb, unit, nullptr };
	run(argv);
}

/*** the name ***/

// The portable user name a Debian useradd accepts without --badnames: a lower
// case letter or underscore, then lower case letters, digits, underscores and
// hyphens. Samba and the Unix tools disagree about case, so one case only.
static bool portable_user_name(const std::string &name) {
	if (name.empty() || name.size() > 32)
		return false;
	if (!(islower((unsigned char) name[0]) || name[0] == '_'))
		return false;
	for (size_t i = 1; i < name.size(); i++) {
		char c = name[i];
		if (!(islower((unsigned char) c) || isdigit((unsigned char) c)
				|| c == '_' || c == '-'))
			return false;
	}
	return true;
}

// True when pw is an account this service created, told by the mark in its
// GECOS field. useradd writes the comment as given; a later chfn may append
// the other GECOS fields behind commas, so only the first one is compared.
static bool is_operator_account(const struct passwd *pw) {
	if (pw == nullptr || pw->pw_gecos == nullptr)
		return false;
	const char *comma = strchr(pw->pw_gecos, ',');
	size_t len = comma != nullptr ? (size_t) (comma - pw->pw_gecos)
			: strlen(pw->pw_gecos);
	return len == strlen(OPERATOR_MARK)
			&& strncmp(pw->pw_gecos, OPERATOR_MARK, len) == 0;
}

bool webshares_name_acceptable(const std::string &name, std::string *error) {
	if (!portable_user_name(name)) {
		*error = "a user name is 1 to 32 characters: a lower case letter or "
				"underscore, then lower case letters, digits, underscores and "
				"hyphens";
		return false;
	}
	for (int i = 0; RESERVED_NAMES[i] != nullptr; i++) {
		if (name == RESERVED_NAMES[i]) {
			*error = "\"" + name + "\" is a name this QUniLator keeps for itself";
			return false;
		}
	}
	struct passwd *pw = getpwnam(name.c_str());
	if (pw != nullptr && !is_operator_account(pw)) {
		*error = "\"" + name + "\" is already an account here, and not one this "
				"service created; choose another name, or adopt that account with "
				"\"--setup-operator " + name + " --adopt-account\" on the card itself";
		return false;
	}
	return true;
}

/*** the share configuration ***/

static bool read_file(const char *path, std::string *out) {
	FILE *f = fopen(path, "rb");
	if (f == nullptr)
		return false;
	char buf[4096];
	size_t n;
	out->clear();
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	fclose(f);
	return true;
}

// Replace path atomically, keeping mode 0644 so the daemons that read it as
// their own user still can.
static bool write_file(const char *path, const std::string &content) {
	std::string tmp = std::string(path) + ".qunilator-new";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
	if (fclose(f) != 0)
		ok = false;
	if (ok)
		ok = chmod(tmp.c_str(), 0644) == 0 && rename(tmp.c_str(), path) == 0;
	if (!ok)
		unlink(tmp.c_str());
	return ok;
}

// Rewrite every line whose text, ignoring the indentation, is exactly from,
// keeping that indentation. Result is true when the file changed.
static bool rewrite_lines(const char *path, const std::string &from,
		const std::string &to) {
	std::string text;
	if (!read_file(path, &text))
		return false;
	std::string out;
	bool changed = false;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t eol = text.find('\n', pos);
		size_t end = eol == std::string::npos ? text.size() : eol;
		std::string line = text.substr(pos, end - pos);
		size_t first = line.find_first_not_of(" \t");
		size_t last = line.find_last_not_of(" \t\r");
		if (first != std::string::npos
				&& line.compare(first, last - first + 1, from) == 0) {
			out += line.substr(0, first) + to;
			changed = true;
		} else {
			out += line;
		}
		if (eol == std::string::npos)
			break;
		out += '\n';
		pos = eol + 1;
	}
	if (!changed)
		return false;
	return write_file(path, out);
}

// Samba's share answers the qunilator group, so both accounts reach it while
// "force user" keeps every file it writes owned by the service account.
static void point_smb_at_the_group(void) {
	if (rewrite_lines(SMB_CONF, "valid users = " WEBSHARES_SERVICE_USER,
			"valid users = @" WEBSHARES_SERVICE_USER)) {
		WEB_INFO("the SMB share now answers the " WEBSHARES_SERVICE_USER " group");
		reload_unit("reload", "smbd");
	}
}

// The Match line sshd confines an SFTP session with, and the earlier forms a
// installation may still carry. Applied in order, so one at any of them arrives at
// the last: the session of a qunilator-group member is confined to the image
// tree unless the account is also an admin, which is what leaves the operator
// a shell while a share-only account keeps none.
static const char *SFTP_MATCH_STEPS[][2] = {
	{ "Match User " WEBSHARES_SERVICE_USER,
	  "Match Group " WEBSHARES_SERVICE_USER },
	{ "Match Group " WEBSHARES_SERVICE_USER,
	  "Match Group " WEBSHARES_SERVICE_USER ",!" WEBSHARES_ADMIN_GROUP },
};

// A configuration sshd refuses to parse would take the QUniLator off ssh at the
// next restart, so the change is checked and dropped if it fails.
static void point_sftp_at_the_group(void) {
	DIR *dir = opendir(SSHD_CONF_DIR);
	if (dir == nullptr)
		return;
	std::vector<std::string> files;
	struct dirent *ent;
	while ((ent = readdir(dir)) != nullptr) {
		std::string name = ent->d_name;
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".conf") == 0)
			files.push_back(std::string(SSHD_CONF_DIR) + "/" + name);
	}
	closedir(dir);
	const size_t steps = sizeof(SFTP_MATCH_STEPS) / sizeof(SFTP_MATCH_STEPS[0]);
	for (size_t i = 0; i < files.size(); i++) {
		std::string before;
		if (!read_file(files[i].c_str(), &before))
			continue;
		bool changed = false;
		for (size_t s = 0; s < steps; s++)
			if (rewrite_lines(files[i].c_str(), SFTP_MATCH_STEPS[s][0],
					SFTP_MATCH_STEPS[s][1]))
				changed = true;
		if (!changed)
			continue;
		const char *argv[] = { SSHD, "-t", nullptr };
		if (have(SSHD) && run(argv) != 0) {
			write_file(files[i].c_str(), before);
			WEB_WARNING("sshd refused the SFTP configuration for the "
					WEBSHARES_SERVICE_USER " group; %s is unchanged",
					files[i].c_str());
			continue;
		}
		WEB_INFO("SFTP now confines the " WEBSHARES_SERVICE_USER
				" group to the image tree, admins excepted");
		reload_unit("reload", "ssh");
	}
}

// vsftpd admits the names its user list holds, and reads the list when it
// starts, so the list is written and the daemon restarted. The operator is the
// only name on it.
static void point_ftp_at(const std::string &name) {
	std::string list = name + "\n";
	std::string current;
	if (!read_file(FTP_USERLIST, &current) || current == list)
		return;
	if (!write_file(FTP_USERLIST, list))
		return;
	WEB_INFO("the FTP user list now admits %s", name.c_str());
	reload_unit("restart", "vsftpd");
}

/*** the account ***/

static void set_unix_password(const std::string &name, const std::string &password) {
	if (!have(CHPASSWD))
		return;
	const char *argv[] = { CHPASSWD, nullptr };
	run_fed(argv, name + ":" + password + "\n");
}

static void set_samba_password(const std::string &name, const std::string &password) {
	if (!have(SMBPASSWD))
		return;
	const char *argv[] = { SMBPASSWD, "-s", "-a", name.c_str(), nullptr };
	run_fed(argv, password + "\n" + password + "\n");
}

// The group whose members may sudo. The package creates it; an installation whose
// package predates it gets it here, so the account is never put in a group that
// is not there.
static void ensure_admin_group(void) {
	if (getgrnam(WEBSHARES_ADMIN_GROUP) != nullptr || !have(GROUPADD))
		return;
	const char *argv[] = { GROUPADD, "--system", WEBSHARES_ADMIN_GROUP, nullptr };
	if (run(argv) == 0)
		WEB_INFO("created the " WEBSHARES_ADMIN_GROUP " group");
}

// The image tree, reachable from the account's own home. The share protocols
// open there by themselves; this is for the shell session, which starts in the
// home directory.
static void link_images_into(const std::string &home, const struct passwd *pw) {
	std::string link = home + "/images";
	struct stat st;
	if (lstat(link.c_str(), &st) == 0)
		return;
	if (symlink(IMAGES_DIR, link.c_str()) != 0) {
		WEB_WARNING("could not link %s to the image tree: %s", link.c_str(),
				strerror(errno));
		return;
	}
	// everything in the home belongs to the account, this link included
	if (lchown(link.c_str(), pw->pw_uid, pw->pw_gid) != 0)
		WEB_WARNING("could not give %s to %s", link.c_str(), pw->pw_name);
}

// True when a shell reaches no session: the operator needs one that does, and
// any other shell is a choice already made and left alone.
static bool no_login_shell(const char *shell) {
	if (shell == nullptr || *shell == 0)
		return true;
	static const char *refuses[] = { "/usr/sbin/nologin", "/sbin/nologin",
			"/bin/false", "/usr/bin/false", nullptr };
	for (int i = 0; refuses[i] != nullptr; i++)
		if (strcmp(shell, refuses[i]) == 0)
			return true;
	return false;
}

// Bring an account to the shape described at the top of this file: a home of
// its own, a shell that reaches a session, and membership of the group that
// carries the image tree and of the one that carries sudo.
static void adopt_account(const struct passwd *pw, const std::string &name,
		const std::string &home) {
	bool wrong_home = pw->pw_dir == nullptr || home != pw->pw_dir;
	bool wrong_shell = no_login_shell(pw->pw_shell);
	if (!have(USERMOD))
		return;
	if (wrong_home || wrong_shell) {
		// --home without --move: the home this account had is the image tree,
		// which stays where it is and belongs to the service account.
		const char *shell = wrong_shell ? OPERATOR_SHELL : pw->pw_shell;
		const char *argv[] = { USERMOD, "--home", home.c_str(),
				"--shell", shell, name.c_str(), nullptr };
		if (run(argv) != 0) {
			WEB_WARNING("could not give %s a home and a shell", name.c_str());
			return;
		}
		WEB_INFO("%s now has a login shell and a home of its own", name.c_str());
	}
	static const char *both_groups = WEBSHARES_SERVICE_USER "," WEBSHARES_ADMIN_GROUP;
	const char *argv[] = { USERMOD, "--append", "--groups", both_groups,
			name.c_str(), nullptr };
	run(argv);
}

// The account's home, created when useradd did not make it - which is the case
// for an account adopted from the share-only shape.
static void ensure_home(const std::string &name, const std::string &home) {
	struct passwd *pw = getpwnam(name.c_str());
	if (pw == nullptr)
		return;
	struct stat st;
	if (stat(home.c_str(), &st) != 0) {
		// 0700, which is what useradd gives a home it creates here, so an
		// adopted account's ssh key is no more readable than a new one's
		if (mkdir(home.c_str(), 0700) != 0) {
			WEB_WARNING("could not create %s: %s", home.c_str(), strerror(errno));
			return;
		}
	}
	if (chown(home.c_str(), pw->pw_uid, pw->pw_gid) != 0)
		WEB_WARNING("could not give %s to %s", home.c_str(), name.c_str());
	link_images_into(home, pw);
}

// Create the operator's account when it is not there yet. Result is true when
// an account by that name exists afterwards, in the shape this file describes.
static bool ensure_account(const std::string &name) {
	ensure_admin_group();
	std::string home = std::string(HOME_BASE) + "/" + name;
	struct passwd *pw = getpwnam(name.c_str());
	if (pw != nullptr) {
		adopt_account(pw, name, home);
		ensure_home(name, home);
		return true;
	}
	if (!have(USERADD)) {
		WEB_WARNING("no useradd on this host: the file shares keep answering "
				"only " WEBSHARES_SERVICE_USER);
		return false;
	}
	const char *argv[] = {
		USERADD, "--create-home", "--home-dir", home.c_str(),
		"--gid", WEBSHARES_SERVICE_USER, "--groups", WEBSHARES_ADMIN_GROUP,
		"--shell", OPERATOR_SHELL,
		"--comment", OPERATOR_MARK, name.c_str(), nullptr
	};
	int status = run(argv);
	if (status != 0 || getpwnam(name.c_str()) == nullptr) {
		WEB_WARNING("useradd %s failed with status %d; the file shares keep "
				"answering only " WEBSHARES_SERVICE_USER, name.c_str(), status);
		return false;
	}
	ensure_home(name, home);
	WEB_INFO("created the operator account %s", name.c_str());
	return true;
}

// True when an account has no usable password: the hash field is empty, or
// carries the "!" or "*" that says a hash is there but withheld. /etc/shadow is
// read as a file rather than through getspnam, which keeps this translation
// unit compiling on the development host the tests run on. An account with no
// line to read counts as locked, so nothing is done to it.
static bool password_is_locked(const char *name) {
	std::string shadow;
	if (!read_file(SHADOW_FILE, &shadow))
		return true;
	std::string prefix = std::string(name) + ":";
	size_t pos = 0;
	while (pos < shadow.size()) {
		size_t eol = shadow.find('\n', pos);
		size_t end = eol == std::string::npos ? shadow.size() : eol;
		if (shadow.compare(pos, prefix.size(), prefix) == 0) {
			char hash = pos + prefix.size() < end ? shadow[pos + prefix.size()] : 0;
			return hash == 0 || hash == ':' || hash == '!' || hash == '*';
		}
		if (eol == std::string::npos)
			break;
		pos = eol + 1;
	}
	return true;
}

// Withdraw the shared onboarding password once the operator has a login of
// their own. Called from where that login is created, so there is never
// left with neither.
static void lock_onboarding_account(const std::string &operator_name) {
	if (operator_name == ONBOARDING_ACCOUNT || !have(USERMOD))
		return;
	if (getpwnam(ONBOARDING_ACCOUNT) == nullptr
			|| password_is_locked(ONBOARDING_ACCOUNT))
		return;
	const char *argv[] = { USERMOD, "--lock", ONBOARDING_ACCOUNT, nullptr };
	if (run(argv) != 0) {
		WEB_WARNING("could not lock the %s account's password", ONBOARDING_ACCOUNT);
		return;
	}
	WEB_INFO("%s now has a login of its own, so the shared %s password is "
			"locked; \"sudo usermod --unlock %s\" restores it",
			operator_name.c_str(), ONBOARDING_ACCOUNT, ONBOARDING_ACCOUNT);
}

// The service account owns the image tree and runs the emulator, and nobody
// logs in as it: the operator's account is what every route answers. An
// installation that gave it the web password before is brought to that here, so
// one credential names one account.
static void lock_service_account(void) {
	if (have(SMBPASSWD)) {
		const char *drop[] = { SMBPASSWD, "-x", WEBSHARES_SERVICE_USER, nullptr };
		run(drop); // no entry to drop is a refusal, and says all is well
	}
	if (!have(USERMOD) || password_is_locked(WEBSHARES_SERVICE_USER))
		return;
	const char *argv[] = { USERMOD, "--lock", WEBSHARES_SERVICE_USER, nullptr };
	if (run(argv) != 0) {
		WEB_WARNING("could not lock the " WEBSHARES_SERVICE_USER " account's password");
		return;
	}
	WEB_INFO("the " WEBSHARES_SERVICE_USER " account no longer takes a login of its own");
}

// Remove an account this service created. A name that is not ours, or is the
// one now in force, is left alone.
static void retire_account(const std::string &name, const std::string &keep) {
	if (name.empty() || name == keep || name == WEBSHARES_SERVICE_USER
			|| name == "root")
		return;
	struct passwd *pw = getpwnam(name.c_str());
	if (pw == nullptr || !is_operator_account(pw))
		return;
	if (have(SMBPASSWD)) {
		const char *argv[] = { SMBPASSWD, "-x", name.c_str(), nullptr };
		run(argv);
	}
	if (have(USERDEL)) {
		// The home goes with the account only when it is the account's own,
		// below /home. An account made before the operator had a shell has the
		// image tree as its home, and that tree is the point of the machine.
		std::string own_home = std::string(HOME_BASE) + "/" + name;
		bool remove_home = pw->pw_dir != nullptr && own_home == pw->pw_dir;
		const char *argv[] = { USERDEL, remove_home ? "--remove" : name.c_str(),
				remove_home ? name.c_str() : nullptr, nullptr };
		int status = run(argv);
		if (status != 0) {
			WEB_WARNING("userdel %s failed with status %d", name.c_str(), status);
			return;
		}
	}
	WEB_INFO("removed the operator account %s", name.c_str());
}

/*** the ssh key ***/

// The key types OpenSSH offers, which is what an authorized_keys line may open
// with. A line naming anything else is refused here rather than written and
// ignored by sshd.
static const char *KEY_TYPES[] = {
	"ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ssh-rsa",
	"ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
	"sk-ecdsa-sha2-nistp256@openssh.com", nullptr
};

// One authorized_keys line: a type, a base64 blob and an optional comment.
// Options before the type are not accepted - a key from a workstation's
// .pub file carries none, and what they can express (command=, tunnel=) is not
// something an operator should be pasting in unseen.
static bool acceptable_ssh_key(const std::string &key, std::string *cleaned,
		std::string *error) {
	std::string k = key;
	while (!k.empty() && (k[k.size() - 1] == '\n' || k[k.size() - 1] == '\r'
			|| k[k.size() - 1] == ' ' || k[k.size() - 1] == '\t'))
		k.erase(k.size() - 1);
	size_t start = k.find_first_not_of(" \t");
	if (start == std::string::npos) {
		*error = "the key is empty";
		return false;
	}
	k = k.substr(start);
	if (k.find('\n') != std::string::npos || k.find('\r') != std::string::npos) {
		*error = "an ssh public key is a single line";
		return false;
	}
	size_t sp = k.find(' ');
	if (sp == std::string::npos) {
		*error = "an ssh public key is a type and a key, separated by a space";
		return false;
	}
	std::string type = k.substr(0, sp);
	bool known = false;
	for (int i = 0; KEY_TYPES[i] != nullptr; i++)
		if (type == KEY_TYPES[i])
			known = true;
	if (!known) {
		*error = "\"" + type + "\" is not an ssh public key type; paste the "
				"contents of a .pub file";
		return false;
	}
	std::string body = k.substr(sp + 1);
	size_t body_end = body.find(' ');
	if (body_end == std::string::npos)
		body_end = body.size();
	if (body_end < 16) {
		*error = "the key itself is missing";
		return false;
	}
	for (size_t i = 0; i < body_end; i++) {
		char c = body[i];
		if (!(isalnum((unsigned char) c) || c == '+' || c == '/' || c == '=')) {
			*error = "the key is not base64; paste the contents of a .pub file";
			return false;
		}
	}
	*cleaned = k;
	return true;
}

bool webshares_set_ssh_key(const std::string &name, const std::string &key,
		std::string *error) {
	if (getuid() != 0) {
		*error = "only an installation on a card can be given an ssh key";
		return false;
	}
	struct passwd *pw = getpwnam(name.c_str());
	if (pw == nullptr || !is_operator_account(pw)) {
		*error = "there is no operator account to give a key to; set a user "
				"name first";
		return false;
	}
	// One key or several: an authorized_keys file is a list, and somebody with
	// a workstation and a laptop has two. Every line is checked before any of
	// them is written, so a file with one bad line among four changes nothing.
	std::vector<std::string> cleaned;
	size_t at = 0;
	while (at <= key.size()) {
		size_t eol = key.find('\n', at);
		std::string line = key.substr(at, eol == std::string::npos
				? std::string::npos : eol - at);
		at = eol == std::string::npos ? key.size() + 1 : eol + 1;
		// blank lines and comments are what an authorized_keys file carries
		// besides keys
		size_t first = line.find_first_not_of(" \t\r");
		if (first == std::string::npos || line[first] == '#')
			continue;
		std::string one;
		if (!acceptable_ssh_key(line, &one, error))
			return false;
		cleaned.push_back(one);
	}
	if (cleaned.empty()) {
		*error = "no ssh key in what was given";
		return false;
	}

	std::string home = pw->pw_dir != nullptr ? pw->pw_dir : "";
	if (home.empty() || home == IMAGES_DIR) {
		*error = "the operator account has no home directory of its own";
		return false;
	}
	std::string dir = home + "/.ssh";
	if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
		*error = std::string("could not create ") + dir + ": " + strerror(errno);
		return false;
	}
	if (chown(dir.c_str(), pw->pw_uid, pw->pw_gid) != 0)
		WEB_WARNING("could not give %s to %s", dir.c_str(), name.c_str());

	// The keys replace whatever was there: what the interface shows and what
	// the account answers to are the same thing.
	std::string path = dir + "/authorized_keys";
	std::string tmp = path + ".qunilator-new";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (f == nullptr) {
		*error = std::string("could not write ") + path + ": " + strerror(errno);
		return false;
	}
	std::string line;
	for (size_t i = 0; i < cleaned.size(); i++)
		line += cleaned[i] + "\n";
	bool ok = fwrite(line.data(), 1, line.size(), f) == line.size();
	if (fclose(f) != 0)
		ok = false;
	if (ok)
		ok = chmod(tmp.c_str(), 0600) == 0
				&& chown(tmp.c_str(), pw->pw_uid, pw->pw_gid) == 0
				&& rename(tmp.c_str(), path.c_str()) == 0;
	if (!ok) {
		unlink(tmp.c_str());
		*error = std::string("could not write ") + path;
		return false;
	}
	WEB_INFO("%s answers %u ssh key%s", name.c_str(), (unsigned) cleaned.size(),
			cleaned.size() == 1 ? "" : "s");
	return true;
}

bool webshares_has_ssh_key(const std::string &name) {
	struct passwd *pw = getpwnam(name.c_str());
	if (pw == nullptr || pw->pw_dir == nullptr)
		return false;
	std::string path = std::string(pw->pw_dir) + "/.ssh/authorized_keys";
	struct stat st;
	return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

bool webshares_account_exists(const std::string &name) {
	return !name.empty() && getpwnam(name.c_str()) != nullptr;
}

bool webshares_adopt_account(const std::string &name, std::string *error) {
	if (getuid() != 0) {
		*error = "adopting an account is root's to do";
		return false;
	}
	for (int i = 0; RESERVED_NAMES[i] != nullptr; i++) {
		if (name == RESERVED_NAMES[i]) {
			*error = "\"" + name + "\" is a name this QUniLator keeps for itself";
			return false;
		}
	}
	struct passwd *pw = getpwnam(name.c_str());
	if (pw == nullptr) {
		*error = "there is no account called \"" + name + "\" to adopt";
		return false;
	}
	if (is_operator_account(pw))
		return true; // already ours, and the rest of the setup does the shaping
	if (pw->pw_uid < 1000) {
		*error = "\"" + name + "\" is a system account";
		return false;
	}
	if (!have(USERMOD)) {
		*error = "no usermod on this host";
		return false;
	}
	const char *argv[] = { USERMOD, "--comment", OPERATOR_MARK, name.c_str(), nullptr };
	if (run(argv) != 0) {
		*error = "could not mark \"" + name + "\" as the operator's account";
		return false;
	}
	WEB_INFO("%s is now the operator's account; its home and its files stay as "
			"they are", name.c_str());
	return true;
}

// chpasswd takes a crypt(3) hash as readily as a password, which is what lets a
// prepared card set the account up without carrying the password.
static void set_unix_password_hashed(const std::string &name, const std::string &hash) {
	if (!have(CHPASSWD))
		return;
	const char *argv[] = { CHPASSWD, "--encrypted", nullptr };
	run_fed(argv, name + ":" + hash + "\n");
}

// smbpasswd only takes a password, and refuses to add an entry without one, so
// the entry is made with a password nobody will ever use - random bytes, read
// here and discarded - and pdbedit then puts the real NT hash in its place.
static void set_samba_password_hashed(const std::string &name, const std::string &nt_hash) {
	if (!have(SMBPASSWD) || !have(PDBEDIT))
		return;
	uint8_t noise[24];
	std::string throwaway;
	FILE *f = fopen("/dev/urandom", "rb");
	if (f != nullptr && fread(noise, 1, sizeof(noise), f) == sizeof(noise)) {
		static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				"abcdefghijklmnopqrstuvwxyz0123456789";
		for (size_t i = 0; i < sizeof(noise); i++)
			throwaway.push_back(alphabet[noise[i] % 62]);
	}
	if (f != nullptr)
		fclose(f);
	if (throwaway.empty()) {
		WEB_WARNING("no randomness for the file shares' throwaway password");
		return;
	}
	const char *add[] = { SMBPASSWD, "-s", "-a", name.c_str(), nullptr };
	run_fed(add, throwaway + "\n" + throwaway + "\n");
	std::string option = "--set-nt-hash=" + nt_hash;
	const char *set[] = { PDBEDIT, "-u", name.c_str(), option.c_str(), nullptr };
	if (run(set) != 0)
		WEB_WARNING("the file shares did not take the NT hash for %s", name.c_str());
}

// Everything a new operator needs beyond the password itself, which the two
// callers below hold in their own form.
static bool shape_the_account(const std::string &name) {
	point_smb_at_the_group();
	point_sftp_at_the_group();
	if (!ensure_account(name)) {
		WEB_WARNING("the file shares have no account for %s", name.c_str());
		return false;
	}
	return true;
}

static void finish_the_account(const std::string &previous, const std::string &name) {
	lock_onboarding_account(name);
	lock_service_account();
	point_ftp_at(name);
	retire_account(previous, name);
	WEB_INFO("the file shares answer %s with the web password", name.c_str());
}

void webshares_apply(const std::string &previous, const std::string &name,
		const std::string &password) {
	if (name.empty() || getuid() != 0 || getpwnam(WEBSHARES_SERVICE_USER) == nullptr)
		return;
	if (!shape_the_account(name))
		return;
	set_unix_password(name, password);
	set_samba_password(name, password);
	finish_the_account(previous, name);
}

void webshares_apply_hashed(const std::string &previous, const std::string &name,
		const std::string &unix_hash, const std::string &nt_hash) {
	if (name.empty() || getuid() != 0 || getpwnam(WEBSHARES_SERVICE_USER) == nullptr)
		return;
	if (!shape_the_account(name))
		return;
	set_unix_password_hashed(name, unix_hash);
	set_samba_password_hashed(name, nt_hash);
	finish_the_account(previous, name);
}

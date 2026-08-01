/* webshares.cpp: the operator's OS account and the file shares

   Copyright (c) 2026, Hans Huebner
   hans@huebner.org
   MIT license, see webserver.hpp for the full text.

   SMB, FTP and SFTP all serve /var/lib/qunilator/images, and all three
   authenticate against the OS. The operator's web user name is therefore an OS
   account, created here beside the qunilator service account when the name is
   set and removed again when it changes.

   The service account keeps owning the tree and running the emulator. The
   operator's account is a login only: its primary group is qunilator, its home
   is the image tree, and its shell is the nologin that /etc/shells lists so
   vsftpd's PAM stack accepts it. Samba's "force user" keeps every file the
   share writes owned by qunilator, so a name change costs one account and no
   walk over the tree.

   The three share configurations name the qunilator *group*, which both
   accounts belong to, so a name change touches accounts rather than
   configuration. A board installed before this rewrites its files once, on the
   first name that is set, and checks its ssh configuration with sshd -t before
   asking sshd to read it.

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
#include <pwd.h>
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
static const char *OPERATOR_SHELL = "/usr/sbin/nologin";

// Written into the account's GECOS field, and the only thing that makes an
// account ours: a name is refused when it belongs to an account without this
// mark, and only an account carrying it is ever removed.
static const char *OPERATOR_MARK = "QUniLator operator";

static const char *USERADD = "/usr/sbin/useradd";
static const char *USERDEL = "/usr/sbin/userdel";
static const char *CHPASSWD = "/usr/sbin/chpasswd";
static const char *SMBPASSWD = "/usr/bin/smbpasswd";
static const char *SYSTEMCTL = "/usr/bin/systemctl";
static const char *SSHD = "/usr/sbin/sshd";

static const char *SMB_CONF = "/etc/samba/smb.conf";
static const char *FTP_USERLIST = "/etc/vsftpd.userlist";
static const char *SSHD_CONF_DIR = "/etc/ssh/sshd_config.d";

// Names this board keeps for itself, refused whether or not the account
// happens to exist here. Accounts that do exist are refused by the ownership
// check below, so this only has to cover the ones a board might be missing.
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
// systemd are both fine; a board that serves no shares has nothing to reload.
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
			*error = "\"" + name + "\" is a name this board keeps for itself";
			return false;
		}
	}
	struct passwd *pw = getpwnam(name.c_str());
	if (pw != nullptr && !is_operator_account(pw)) {
		*error = "\"" + name + "\" is already an account on this board";
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

// sshd confines the SFTP session of every member of the qunilator group to the
// image tree. A configuration sshd refuses to parse would take the board off
// ssh at the next restart, so the change is checked and dropped if it fails.
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
	for (size_t i = 0; i < files.size(); i++) {
		std::string before;
		if (!read_file(files[i].c_str(), &before))
			continue;
		if (!rewrite_lines(files[i].c_str(), "Match User " WEBSHARES_SERVICE_USER,
				"Match Group " WEBSHARES_SERVICE_USER))
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
				" group to the image tree");
		reload_unit("reload", "ssh");
	}
}

// vsftpd admits the names its user list holds, and reads the list when it
// starts, so the list is written and the daemon restarted.
static void point_ftp_at(const std::string &name) {
	std::string list = std::string(WEBSHARES_SERVICE_USER) + "\n";
	if (!name.empty())
		list += name + "\n";
	std::string current;
	if (!read_file(FTP_USERLIST, &current) || current == list)
		return;
	if (!write_file(FTP_USERLIST, list))
		return;
	WEB_INFO("the FTP user list now admits %s",
			name.empty() ? WEBSHARES_SERVICE_USER : name.c_str());
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

// Create the operator's account when it is not there yet. Result is true when
// an account by that name exists afterwards.
static bool ensure_account(const std::string &name) {
	if (getpwnam(name.c_str()) != nullptr)
		return true;
	if (!have(USERADD)) {
		WEB_WARNING("no useradd on this host: the file shares keep answering "
				"only " WEBSHARES_SERVICE_USER);
		return false;
	}
	const char *argv[] = {
		USERADD, "--no-create-home", "--home-dir", IMAGES_DIR,
		"--gid", WEBSHARES_SERVICE_USER, "--shell", OPERATOR_SHELL,
		"--comment", OPERATOR_MARK, name.c_str(), nullptr
	};
	int status = run(argv);
	if (status != 0 || getpwnam(name.c_str()) == nullptr) {
		WEB_WARNING("useradd %s failed with status %d; the file shares keep "
				"answering only " WEBSHARES_SERVICE_USER, name.c_str(), status);
		return false;
	}
	WEB_INFO("created the file-share account %s", name.c_str());
	return true;
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
		const char *argv[] = { USERDEL, name.c_str(), nullptr };
		int status = run(argv);
		if (status != 0) {
			WEB_WARNING("userdel %s failed with status %d", name.c_str(), status);
			return;
		}
	}
	WEB_INFO("removed the file-share account %s", name.c_str());
}

void webshares_apply(const std::string &previous, const std::string &name,
		const std::string &password) {
	if (getuid() != 0 || getpwnam(WEBSHARES_SERVICE_USER) == nullptr)
		return;

	// The service account's password follows the web password whatever the
	// operator is called, so a board with no name set behaves as it always has
	// and one with a name keeps a way in under its own account.
	set_unix_password(WEBSHARES_SERVICE_USER, password);
	set_samba_password(WEBSHARES_SERVICE_USER, password);

	std::string in_force;
	if (!name.empty()) {
		point_smb_at_the_group();
		point_sftp_at_the_group();
		if (ensure_account(name)) {
			set_unix_password(name, password);
			set_samba_password(name, password);
			in_force = name;
		}
	}
	point_ftp_at(in_force);
	retire_account(previous, in_force);

	if (in_force.empty())
		WEB_INFO("the file shares answer " WEBSHARES_SERVICE_USER
				" with the web password");
	else
		WEB_INFO("the file shares answer %s with the web password",
				in_force.c_str());
}

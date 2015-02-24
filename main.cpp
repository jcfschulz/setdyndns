/* Yup, this code is dirty and you probably shouldn't use it
   for anything except of learning purposes.
   
   You have been warned. */

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <regex.h>
#include <grp.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>

#include <ext/stdio_filebuf.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define RE_OCTET "([0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])"
#define RE_IPV4 RE_OCTET "\\." RE_OCTET "\\." RE_OCTET "\\." RE_OCTET

const char * const cg_dyndns_group = "dyndns";
const char * const cg_dyndns_zonefile = "/etc/bind/zones/dyndns.myhost.example.zone";

char **g_argv;

int usage(const std::string &msg) {
	if (!msg.empty()) {
		std::cerr << msg << "\n\n";
	}

	std::cerr
		<< g_argv[0] << "\n"
			"\tsets the users IP address to his current address.\n"
		<< g_argv[0] << " A <ip-addr>\n"
			"\tsets the users IP address to <ip-addr>.\n";

	std::exit(EXIT_FAILURE);
}

bool checkip(const char *ipaddr) {
	regex_t re_ipv4;
	if (regcomp(&re_ipv4,
		"^" RE_IPV4 "$",
		REG_EXTENDED | REG_NOSUB
	)) {
		usage("Regex compilation failed.");
	}
	int result = regexec(&re_ipv4, ipaddr, 0, nullptr, 0);
	regfree(&re_ipv4);

	return !result;
}

bool checkgroup(const char *username) {
	static group *grp = getgrnam(cg_dyndns_group);
	if (!grp) return false;

	for(const char * const *member = grp->gr_mem; *member; ++member) {
		if (!strcmp(username, *member)) {
			return true;
		}
	}
	return false;
}

bool update_dyndns_worker(int fd, const std::string &username, const std::string &ipaddr) {
	std::string content;
	{
		std::string line;
		const std::string new_record = username + "\tIN\tA\t" + ipaddr + "\n";

		// GCC extension!!
		__gnu_cxx::stdio_filebuf<char> zonefile_buf(dup(fd), std::ios::in | std::ios::out);
		std::istream zonefile(&zonefile_buf);

		bool update_serial = false, update_record = false;

		while(zonefile) {
			std::getline(zonefile, line);
			std::stringstream ss(line);

			std::string word;
			ss >> word;

			if (word == ";ZONE-VERSION") {
				content += line + "\n";

				std::getline(zonefile, line);
				ss.str(line);
				long serial = -1;
				ss >> serial;
				if (update_serial || serial < 0) {
					std::cerr << "Failure while reading serial. Aborting.\n";
					return false;
				}

				std::stringstream msg;
				msg << "\t" << (serial+1) << " ; set by setdyndns\n";
				content += msg.str();

				update_serial = true;
			}

			else if (word == username) {
				update_record = true;
				content += new_record;
			}

			else {
				content += line + "\n";
			}
		}

		if (!update_serial) {
			std::cerr << "Failure while updating serial. Aborting.\n";
			return false;
		}

		content.erase(content.find_last_not_of("\r\n\t ")+1);
		content += "\n"; // extra, as first ws in prev. line may not have been \n

		if (!update_record) {
			content += new_record;
		}
	}

	lseek(fd, 0, SEEK_SET);
	if(0 == ftruncate(fd, 0) && content.size() == write(fd, content.data(), content.size())) {
		return true;
	}
	else {
		std::cerr << "Error while writing new zone file.\n";
		return false;
	}
}

bool update_dyndns(const std::string &username, const std::string ipaddr) {
	int fd = open(cg_dyndns_zonefile, O_RDWR), lock_attempts=3;
	if (fd < 0) {
		std::cerr << "Could not open zone file. Aborting.\n";
		return false;
	}

	bool retval = false;
	bool haslock = false;
	flock lockinfo;
		lockinfo.l_type  = F_WRLCK; // write lock
		lockinfo.l_whence = SEEK_SET; // absolute position
		lockinfo.l_start = 0; // from start
		lockinfo.l_len   = 0; // to end

	while(lock_attempts--) {
		if (-1 != fcntl(fd, F_SETLK, &lockinfo)) {
			haslock = true;
		}
		else if (lock_attempts && (errno == EAGAIN || errno == EACCES)) {
			usleep(150*1000); // 150msec
		}
		else {
			break; // permanent error
		}
	}

	if (haslock) {
		retval = update_dyndns_worker(fd, username, ipaddr);
		lockinfo.l_type = F_UNLCK;
		fcntl(fd, F_SETLK, &lockinfo);	
	}
	else {
		std::cerr << "Failed to obtain lock on zone file. Aborting.\n";
	}

	close(fd);
	return retval;
}

bool restart_named() {
	setuid(0); // become root to bind to restricted ports
	int status = std::system("/etc/init.d/bind9 restart >/dev/null");
	return (status != -1 && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv) {
	g_argv = argv;

	std::string
		username,
		ipaddr;
  struct passwd *pw;
  uid_t uid;


	if (argc == 3) {
		// Check argument for valid IP address
		if (std::strcmp("A", argv[1])) {
			usage("Only 'A' records may e updated currently.");
		}

		ipaddr = argv[2];
	}
	else if (argc == 1) {
		const char *sshconn = getenv("SSH_CONNECTION");
		if (!sshconn) {
			std::cerr <<
				"$SSH_CONNECTION not set.\n"
				"Automatic IP detection failed.\n";
			return EXIT_FAILURE;
		}
		ipaddr = sshconn;
		std::string::size_type pos =
			ipaddr.find_first_not_of("0123456789.");
		if (pos != ipaddr.npos) {
			ipaddr.erase(pos);
		}
	}
	else {
		usage("Invalid number of parameters.");
	}

	if (!checkip(ipaddr.c_str())) {
		usage(
			"Invalid IP address: '" + ipaddr + "'.\n"
			"Supplied parameter must be a valid IP address "
			"without any leading zeroes."
		);
	}

  uid = getuid();
  pw = getpwuid(uid);
  username = pw->pw_name; 

	if (!checkgroup(username.c_str())) {
		std::cerr << argv[0]
			<< " can only be called by members of group '"
			<< cg_dyndns_group << "'." << std::endl;
		return EXIT_FAILURE;
	}

	if (update_dyndns(username, ipaddr) && restart_named()) {
		std::cout
			<< "DynDNS entry for " << username
			<< " changed to " << ipaddr << ".\n";
		return EXIT_SUCCESS;
	}
	else {
		std::cerr
			<< "DynDNS entry not changed.\n";
		return EXIT_FAILURE;
	}
}

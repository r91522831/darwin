/*
 *   LinuxNetwork.h
 *
 *   Author: ROBOTIS
 *
 */

#ifndef _LINUX_NETWORK_H_
#define _LINUX_NETWORK_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string>
#include <arpa/inet.h>


namespace Robot
{
	class LinuxSocket
	{
		private:
			int m_sock;
			sockaddr_in m_addr;
			bool non_blocking;

		public:
			static const int MAXHOSTNAME = 200;
			static const int MAXCONNECTIONS = 5;
			static const int MAXRECV = 500;

			LinuxSocket();
			virtual ~LinuxSocket();

			// Server initialization
			bool create();
			bool bind ( const char* hostname, const int port );
			bool listen() const;
			bool accept ( LinuxSocket& ) const;

			// Client initialization
			bool connect ( const std::string host, const int port );

			// Data Transimission
			bool send ( const std::string ) const;
			bool send ( void* data, int length ) const;
			int recv ( std::string& ) const;
			int recv ( void* data, int length ) const;

			void set_non_blocking ( const bool );
			bool selectRead(int tmout) const;
			bool selectWrite(int tmout) const;

			bool is_valid() const { return m_sock != -1; }
			void close();
	};

	class LinuxSocketException 
	{
		private:
			std::string m_s;

		public:
			LinuxSocketException ( std::string s ) : m_s ( s ) {};
			~LinuxSocketException (){};

			std::string description() { return m_s; }
	};

	class LinuxServer : private LinuxSocket
	{
		public:
			LinuxServer ( const char* hostname, int port );
			LinuxServer (){};
			virtual ~LinuxServer();
			void set_non_blocking(const bool b);

			const LinuxServer& operator << ( const std::string& ) const;
			const LinuxServer& operator << ( const int& ) const;
			const LinuxServer& operator >> ( std::string& ) const;

			void accept ( LinuxServer& );
			bool send ( unsigned char *data, int length );
			int recv ( unsigned char *data, int length );

			bool valid() { return LinuxSocket::is_valid(); }
			void close();
	};
}

#endif

/*
 *   CM730.cpp
 *
 *   Author: ROBOTIS
 *
 */
#include <stdio.h>
#include "FSR.h"
#include "CM730.h"
#include "MotionStatus.h"

#include <time.h>

using namespace Robot;


#define ID					(2)
#define LENGTH				(3)
#define INSTRUCTION			(4)
#define ERRBIT				(4)
#define PARAMETER			(5)
#define DEFAULT_BAUDNUMBER	(1)

#define INST_PING			(1)
#define INST_READ			(2)
#define INST_WRITE			(3)
#define INST_REG_WRITE		(4)
#define INST_ACTION			(5)
#define INST_RESET			(6)
#define INST_SYNC_WRITE		(131)   // 0x83
#define INST_BULK_READ      (146)   // 0x92


BulkReadData::BulkReadData() :
	start_address(0),
	length(0),
	error(-1)
{
	for(int i = 0; i < MX28::MAXNUM_ADDRESS; i++)
		table[i] = 0;
}

int BulkReadData::ReadByte(int address)
{
	if(address >= start_address && address < (start_address + length))
		return (int)table[address];

	return 0;
}

int BulkReadData::ReadWord(int address)
{
	if(address >= start_address && address < (start_address + length))
		return CM730::MakeWord(table[address], table[address+1]);

	return 0;
}

CM730::CM730(PlatformCM730 *platform)
{
	m_Platform = platform;
	DEBUG_PRINT = false;
	for(int i = 0; i < ID_BROADCAST; i++)
		m_BulkReadData[i] = BulkReadData();
}

CM730::~CM730()
{
	Disconnect();
}

static double ms_diff(timespec start, timespec end)
{
	timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) {
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return (double)(temp.tv_sec*1000.0+temp.tv_nsec/1000000.0);
}

int CM730::OneTxRxPacket(unsigned char *txpacket, unsigned char *rxpacket, int priority)
{
	if(priority > 1)
		m_Platform->LowPriorityWait();
	if(priority > 0)
		m_Platform->MidPriorityWait();
	m_Platform->HighPriorityWait();

	int res = TX_FAIL;
	int length = txpacket[LENGTH] + 4;

	txpacket[0] = 0xFF;
	txpacket[1] = 0xFF;
	txpacket[length - 1] = CalculateChecksum(txpacket);

	static struct timespec start_time;
	static struct timespec write_time;
	static struct timespec read_time;
	static struct timespec parse_time;

	if(length < (MAXNUM_TXPARAM + 6))
	{
		m_Platform->ClearPort();
		clock_gettime(CLOCK_MONOTONIC, &start_time);
		if(m_Platform->WritePort(txpacket, length) == length)
		{
			clock_gettime(CLOCK_MONOTONIC, &write_time);
			if (txpacket[ID] != ID_BROADCAST)
			{
				int to_length = 0;

				if(txpacket[INSTRUCTION] == INST_READ)
					to_length = txpacket[PARAMETER+1] + 6;
				else
					to_length = 6;

				m_Platform->SetPacketTimeout(length);

				int get_length = 0;

				while(1)
				{
					length = m_Platform->ReadPort(&rxpacket[get_length], to_length - get_length);
					get_length += length;

					if(get_length == to_length)
					{
						// Find packet header
						int i;
						for(i = 0; i < (get_length - 1); i++)
						{
							if(rxpacket[i] == 0xFF && rxpacket[i+1] == 0xFF)
								break;
							else if(i == (get_length - 2) && rxpacket[get_length - 1] == 0xFF)
								break;
						}

						if(i == 0)
						{
							// Check checksum
							unsigned char checksum = CalculateChecksum(rxpacket);

							if(rxpacket[get_length-1] == checksum)
								res = SUCCESS;
							else
								res = RX_CORRUPT;

							break;
						}
						else
						{
							for(int j = 0; j < (get_length - i); j++)
								rxpacket[j] = rxpacket[j+i];
							get_length -= i;
						}						
					}
					else
					{
						if(m_Platform->IsPacketTimeout() == true)
						{
							if(get_length == 0)
								res = RX_TIMEOUT;
							else
								res = RX_CORRUPT;

							break;
						}
					}
				}
				clock_gettime(CLOCK_MONOTONIC, &read_time);
				clock_gettime(CLOCK_MONOTONIC, &parse_time);

				printf("Norm Write: %f\tRead: %f: Parse: %f\n",
						ms_diff(start_time, write_time), 
						ms_diff(write_time, read_time), 
						ms_diff(read_time, parse_time));
			}
			else if(txpacket[INSTRUCTION] == INST_BULK_READ)
			{
				int to_length = 0;
				int num = (txpacket[LENGTH]-3) / 3;

				for(int x = 0; x < num; x++)
				{
					int _id = txpacket[PARAMETER+(3*x)+2];
					int _len = txpacket[PARAMETER+(3*x)+1];
					int _addr = txpacket[PARAMETER+(3*x)+3];

					to_length += _len + 6;
					m_BulkReadData[_id].length = _len;
					m_BulkReadData[_id].start_address = _addr;
				}

				m_Platform->SetPacketTimeout(to_length*1.5);

				int get_length = 0;

				clock_gettime(CLOCK_MONOTONIC, &write_time);
				while(1)
				{
					length = m_Platform->ReadPort(&rxpacket[get_length], to_length - get_length);
					get_length += length;

					if(get_length == to_length)
					{
						res = SUCCESS;
						break;
					}
					else
					{
						if(m_Platform->IsPacketTimeout() == true)
						{
							if(get_length == 0)
								res = RX_TIMEOUT;
							else
								res = RX_CORRUPT;

							break;
						}
					}
				}
				clock_gettime(CLOCK_MONOTONIC, &read_time);

				for(int x = 0; x < num; x++)
				{
					int _id = txpacket[PARAMETER+(3*x)+2];
					m_BulkReadData[_id].error = -1;
				}

				while(1)
				{
					int i;
					for(i = 0; i < get_length - 1; i++)
					{
						if(rxpacket[i] == 0xFF && rxpacket[i+1] == 0xFF)
							break;
						else if(i == (get_length - 2) && rxpacket[get_length - 1] == 0xFF)
							break;
					}

					if(i == 0)
					{
						// Check checksum
						unsigned char checksum = CalculateChecksum(rxpacket);

						if(rxpacket[LENGTH+rxpacket[LENGTH]] == checksum)
						{
							for(int j = 0; j < (rxpacket[LENGTH]-2); j++)
								m_BulkReadData[rxpacket[ID]].table[m_BulkReadData[rxpacket[ID]].start_address + j] = rxpacket[PARAMETER + j];

							m_BulkReadData[rxpacket[ID]].error = (int)rxpacket[ERRBIT];

							int cur_packet_length = LENGTH + 1 + rxpacket[LENGTH];
							to_length = get_length - cur_packet_length;
							for(int j = 0; j <= to_length; j++)
								rxpacket[j] = rxpacket[j+cur_packet_length];

							get_length = to_length;
							num--;
						}
						else
						{
							res = RX_CORRUPT;

							for(int j = 0; j <= get_length - 2; j++)
								rxpacket[j] = rxpacket[j+2];

							to_length = get_length -= 2;
						}

						if(num == 0)
							break;
						else if(get_length <= 6)
						{
							if(num != 0) res = RX_CORRUPT;
							break;
						}

					}
					else
					{
						for(int j = 0; j < (get_length - i); j++)
							rxpacket[j] = rxpacket[j+i];
						get_length -= i;
					}
				}

				clock_gettime(CLOCK_MONOTONIC, &parse_time);

				printf("Bulk Write: %f\tRead: %f: Parse: %f\n",
						ms_diff(start_time, write_time),
						ms_diff(write_time, read_time),
						ms_diff(read_time, parse_time));
			}
			else
			{
				res = SUCCESS;
			}
		}
		else
		{
			res = TX_FAIL;
		}
	}
	else
	{
		res = TX_CORRUPT;
	}

	m_Platform->HighPriorityRelease();
	if(priority > 0)
		m_Platform->MidPriorityRelease();
	if(priority > 1)
		m_Platform->LowPriorityRelease();

	return res;
}

int CM730::TxRxPacket(unsigned char *txpacket, unsigned char *rxpacket, int priority)
{
	if(priority > 1)
		m_Platform->LowPriorityWait();
	if(priority > 0)
		m_Platform->MidPriorityWait();
	m_Platform->HighPriorityWait();

	int res = TX_FAIL;
	int length = txpacket[LENGTH] + 4;

	txpacket[0] = 0xFF;
	txpacket[1] = 0xFF;
	txpacket[length - 1] = CalculateChecksum(txpacket);

	static struct timespec start_time;
	static struct timespec write_time;
	static struct timespec read_time;
	static struct timespec parse_time;

	if(DEBUG_PRINT == true)
	{
		fprintf(stderr, "\nTX: ");
		for(int n=0; n<length; n++)
			fprintf(stderr, "%.2X ", txpacket[n]);

		fprintf(stderr, "INST: ");
		switch(txpacket[INSTRUCTION])
		{
			case INST_PING:
				fprintf(stderr, "PING\n");
				break;

			case INST_READ:
				fprintf(stderr, "READ\n");
				break;

			case INST_WRITE:
				fprintf(stderr, "WRITE\n");
				break;

			case INST_REG_WRITE:
				fprintf(stderr, "REG_WRITE\n");
				break;

			case INST_ACTION:
				fprintf(stderr, "ACTION\n");
				break;

			case INST_RESET:
				fprintf(stderr, "RESET\n");
				break;

			case INST_SYNC_WRITE:
				fprintf(stderr, "SYNC_WRITE\n");
				break;

			case INST_BULK_READ:
				fprintf(stderr, "BULK_READ\n");
				break;

			default:
				fprintf(stderr, "UNKNOWN\n");
				break;
		}
	}

	if(length < (MAXNUM_TXPARAM + 6))
	{
		m_Platform->ClearPort();
		clock_gettime(CLOCK_MONOTONIC, &start_time);
		if(m_Platform->WritePort(txpacket, length) == length)
		{
			clock_gettime(CLOCK_MONOTONIC, &write_time);
			if (txpacket[ID] != ID_BROADCAST)
			{
				int to_length = 0;

				if(txpacket[INSTRUCTION] == INST_READ)
					to_length = txpacket[PARAMETER+1] + 6;
				else
					to_length = 6;

				m_Platform->SetPacketTimeout(length);

				int get_length = 0;
				if(DEBUG_PRINT == true)
					fprintf(stderr, "RX: ");

				while(1)
				{
					length = m_Platform->ReadPort(&rxpacket[get_length], to_length - get_length);
					if(DEBUG_PRINT == true)
					{
						for(int n=0; n<length; n++)
							fprintf(stderr, "%.2X ", rxpacket[get_length + n]);
					}
					get_length += length;

					if(get_length == to_length)
					{
						// Find packet header
						int i;
						for(i = 0; i < (get_length - 1); i++)
						{
							if(rxpacket[i] == 0xFF && rxpacket[i+1] == 0xFF)
								break;
							else if(i == (get_length - 2) && rxpacket[get_length - 1] == 0xFF)
								break;
						}

						if(i == 0)
						{
							// Check checksum
							unsigned char checksum = CalculateChecksum(rxpacket);
							if(DEBUG_PRINT == true)
								fprintf(stderr, "CHK:%.2X\n", checksum);

							if(rxpacket[get_length-1] == checksum)
								res = SUCCESS;
							else
								res = RX_CORRUPT;

							break;
						}
						else
						{
							for(int j = 0; j < (get_length - i); j++)
								rxpacket[j] = rxpacket[j+i];
							get_length -= i;
						}
					}
					else
					{
						if(m_Platform->IsPacketTimeout() == true)
						{
							if(get_length == 0)
								res = RX_TIMEOUT;
							else
								res = RX_CORRUPT;

							break;
						}
					}
				}
			}
			else if(txpacket[INSTRUCTION] == INST_BULK_READ)
			{
				int to_length = 0;
				int num = (txpacket[LENGTH]-3) / 3;

				for(int x = 0; x < num; x++)
				{
					int _id = txpacket[PARAMETER+(3*x)+2];
					int _len = txpacket[PARAMETER+(3*x)+1];
					int _addr = txpacket[PARAMETER+(3*x)+3];

					to_length += _len + 6;
					m_BulkReadData[_id].length = _len;
					m_BulkReadData[_id].start_address = _addr;
				}

				m_Platform->SetPacketTimeout(to_length*1.5);

				int get_length = 0;
				if(DEBUG_PRINT == true)
					fprintf(stderr, "RX: ");

				clock_gettime(CLOCK_MONOTONIC, &write_time);
				while(1)
				{
					length = m_Platform->ReadPort(&rxpacket[get_length], to_length - get_length);
					if(DEBUG_PRINT == true)
					{
						for(int n=0; n<length; n++)
							fprintf(stderr, "%.2X ", rxpacket[get_length + n]);
					}
					get_length += length;

					if(get_length == to_length)
					{
						res = SUCCESS;
						break;
					}
					else
					{
						if(m_Platform->IsPacketTimeout() == true)
						{
							if(get_length == 0)
								res = RX_TIMEOUT;
							else
								res = RX_CORRUPT;

							break;
						}
					}
				}
				clock_gettime(CLOCK_MONOTONIC, &read_time);

				for(int x = 0; x < num; x++)
				{
					int _id = txpacket[PARAMETER+(3*x)+2];
					m_BulkReadData[_id].error = -1;
				}

				while(1)
				{
					int i;
					for(i = 0; i < get_length - 1; i++)
					{
						if(rxpacket[i] == 0xFF && rxpacket[i+1] == 0xFF)
							break;
						else if(i == (get_length - 2) && rxpacket[get_length - 1] == 0xFF)
							break;
					}

					if(i == 0)
					{
						// Check checksum
						unsigned char checksum = CalculateChecksum(rxpacket);
						if(DEBUG_PRINT == true)
							fprintf(stderr, "CHK:%.2X\n", checksum);

						if(rxpacket[LENGTH+rxpacket[LENGTH]] == checksum)
						{
							for(int j = 0; j < (rxpacket[LENGTH]-2); j++)
								m_BulkReadData[rxpacket[ID]].table[m_BulkReadData[rxpacket[ID]].start_address + j] = rxpacket[PARAMETER + j];

							m_BulkReadData[rxpacket[ID]].error = (int)rxpacket[ERRBIT];

							int cur_packet_length = LENGTH + 1 + rxpacket[LENGTH];
							to_length = get_length - cur_packet_length;
							for(int j = 0; j <= to_length; j++)
								rxpacket[j] = rxpacket[j+cur_packet_length];

							get_length = to_length;
							num--;
						}
						else
						{
							res = RX_CORRUPT;

							for(int j = 0; j <= get_length - 2; j++)
								rxpacket[j] = rxpacket[j+2];

							to_length = get_length -= 2;
						}

						if(num == 0)
							break;
						else if(get_length <= 6)
						{
							if(num != 0) res = RX_CORRUPT;
							break;
						}

					}
					else
					{
						for(int j = 0; j < (get_length - i); j++)
							rxpacket[j] = rxpacket[j+i];
						get_length -= i;
					}
				}
			}
			else
			{
				res = SUCCESS;
			}
		}
		else
		{
			res = TX_FAIL;
		}
	}
	else
	{
		res = TX_CORRUPT;
	}

	if(DEBUG_PRINT == true)
	{
		fprintf(stderr, "Time:%.2fms  ", m_Platform->GetPacketTime());
		fprintf(stderr, "RETURN: ");
		switch(res)
		{
			case SUCCESS:
				fprintf(stderr, "SUCCESS\n");
				break;

			case TX_CORRUPT:
				fprintf(stderr, "TX_CORRUPT\n");
				break;

			case TX_FAIL:
				fprintf(stderr, "TX_FAIL\n");
				break;

			case RX_FAIL:
				fprintf(stderr, "RX_FAIL\n");
				break;

			case RX_TIMEOUT:
				fprintf(stderr, "RX_TIMEOUT\n");
				break;

			case RX_CORRUPT:
				fprintf(stderr, "RX_CORRUPT\n");
				break;

			default:
				fprintf(stderr, "UNKNOWN\n");
				break;
		}
	}

	m_Platform->HighPriorityRelease();
	if(priority > 0)
		m_Platform->MidPriorityRelease();
	if(priority > 1)
		m_Platform->LowPriorityRelease();



	return res;
}

unsigned char CM730::CalculateChecksum(unsigned char *packet)
{
	unsigned char checksum = 0x00;
	for(int i=2; i<packet[LENGTH]+3; i++ )
		checksum += packet[i];
	return (~checksum);
}

int CM730::BulkRead()
{
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };

	if(m_BulkReadTxPacket[LENGTH] != 0)
		return TxRxPacket(m_BulkReadTxPacket, rxpacket, 0);
	else
	{
		//MakeBulkReadPacket();
		MakeBulkReadPacketMPC();
		return TX_FAIL;
	}
}

// start_addr for each motor
// each_length = number of things writing per motor (including id #)
// number of motors
int CM730::SyncWrite(int start_addr, int each_length, int number, int *pParam)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int n;

	txpacket[ID]                = (unsigned char)ID_BROADCAST;
	txpacket[INSTRUCTION]       = INST_SYNC_WRITE;
	txpacket[PARAMETER]			= (unsigned char)start_addr;
	txpacket[PARAMETER + 1]		= (unsigned char)(each_length - 1);
	for(n = 0; n < (number * each_length); n++)
		txpacket[PARAMETER + 2 + n]   = (unsigned char)pParam[n];
	txpacket[LENGTH]            = n + 4;

	return TxRxPacket(txpacket, rxpacket, 0);
}

bool CM730::Connect()
{
	if(m_Platform->OpenPort() == false)
	{
		fprintf(stderr, "\n Fail to open port\n");
		fprintf(stderr, " CM-730 is used by another program or do not have root privileges.\n\n");
		return false;
	}

	return DXLPowerOn();
}

bool CM730::ChangeBaud(int baud)
{
	if(m_Platform->SetBaud(baud) == false)
	{
		fprintf(stderr, "\n Fail to change baudrate\n");
		return false;
	}

	return DXLPowerOn();
}

bool CM730::DXLPowerOn()
{
	if(WriteByte(CM730::ID_CM, CM730::P_DXL_POWER, 1, 0) == CM730::SUCCESS)
	{
		if(DEBUG_PRINT == true)
			fprintf(stderr, " Succeed to change Dynamixel power!\n");

		WriteWord(CM730::ID_CM, CM730::P_LED_HEAD_L, MakeColor(255, 128, 0), 0);
		m_Platform->Sleep(300); // about 300msec
	}
	else
	{
		if(DEBUG_PRINT == true)
			fprintf(stderr, " Fail to change Dynamixel power!\n");
		return false;
	}

	return true;
}

void CM730::Disconnect()
{
	// Make the Head LED to green
	//WriteWord(CM730::ID_CM, CM730::P_LED_HEAD_L, MakeColor(0, 255, 0), 0);
	unsigned char txpacket[] = {0xFF, 0xFF, 0xC8, 0x05, 0x03, 0x1A, 0xE0, 0x03, 0x32};
	m_Platform->WritePort(txpacket, 9);

	m_Platform->ClosePort();
}

int CM730::WriteByte(int address, int value, int *error)
{
	return WriteByte(ID_CM, address, value, error);
}

int CM730::WriteWord(int address, int value, int *error)
{
	return WriteWord(ID_CM, address, value, error);
}

int CM730::Ping(int id, int *error)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int result;

	txpacket[ID]           = (unsigned char)id;
	txpacket[INSTRUCTION]  = INST_PING;
	txpacket[LENGTH]       = 2;

	result = TxRxPacket(txpacket, rxpacket, 2);
	if(result == SUCCESS && txpacket[ID] != ID_BROADCAST)
	{		
		if(error != 0)
			*error = (int)rxpacket[ERRBIT];
	}

	return result;
}

int CM730::ReadByte(int id, int address, int *pValue, int *error)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int result;

	txpacket[ID]           = (unsigned char)id;
	txpacket[INSTRUCTION]  = INST_READ;
	txpacket[PARAMETER]    = (unsigned char)address;
	txpacket[PARAMETER+1]  = 1;
	txpacket[LENGTH]       = 4;

	result = TxRxPacket(txpacket, rxpacket, 2);
	if(result == SUCCESS)
	{
		*pValue = (int)rxpacket[PARAMETER];
		if(error != 0)
			*error = (int)rxpacket[ERRBIT];
	}

	return result;
}

int CM730::ReadWord(int id, int address, int *pValue, int *error)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int result;

	txpacket[ID]           = (unsigned char)id;
	txpacket[INSTRUCTION]  = INST_READ;
	txpacket[PARAMETER]    = (unsigned char)address;
	txpacket[PARAMETER+1]  = 2;
	txpacket[LENGTH]       = 4;

	result = TxRxPacket(txpacket, rxpacket, 2);
	if(result == SUCCESS)
	{
		*pValue = MakeWord((int)rxpacket[PARAMETER], (int)rxpacket[PARAMETER + 1]);

		if(error != 0)
			*error = (int)rxpacket[ERRBIT];
	}

	return result;
}

int CM730::ReadTable(int id, int start_addr, int end_addr, unsigned char *table, int *error)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int result;
	int length = end_addr - start_addr + 1;

	txpacket[ID]           = (unsigned char)id;
	txpacket[INSTRUCTION]  = INST_READ;
	txpacket[PARAMETER]    = (unsigned char)start_addr;
	txpacket[PARAMETER+1]  = (unsigned char)length;
	txpacket[LENGTH]       = 4;

	result = TxRxPacket(txpacket, rxpacket, 1);
	if(result == SUCCESS)
	{
		for(int i=0; i<length; i++)
			table[start_addr + i] = rxpacket[PARAMETER + i];

		if(error != 0)
			*error = (int)rxpacket[ERRBIT];
	}

	return result;
}

int CM730::WriteByte(int id, int address, int value, int *error)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int result;

	txpacket[ID]           = (unsigned char)id;
	txpacket[INSTRUCTION]  = INST_WRITE;
	txpacket[PARAMETER]    = (unsigned char)address;
	txpacket[PARAMETER+1]  = (unsigned char)value;
	txpacket[LENGTH]       = 4;

	result = TxRxPacket(txpacket, rxpacket, 2);
	if(result == SUCCESS && id != ID_BROADCAST)
	{
		if(error != 0)
			*error = (int)rxpacket[ERRBIT];
	}

	return result;
}

int CM730::WriteWord(int id, int address, int value, int *error)
{
	unsigned char txpacket[MAXNUM_TXPARAM + 10] = {0, };
	unsigned char rxpacket[MAXNUM_RXPARAM + 10] = {0, };
	int result;

	txpacket[ID]           = (unsigned char)id;
	txpacket[INSTRUCTION]  = INST_WRITE;
	txpacket[PARAMETER]    = (unsigned char)address;
	txpacket[PARAMETER+1]  = (unsigned char)GetLowByte(value);
	txpacket[PARAMETER+2]  = (unsigned char)GetHighByte(value);
	txpacket[LENGTH]       = 5;

	result = TxRxPacket(txpacket, rxpacket, 2);
	if(result == SUCCESS && id != ID_BROADCAST)
	{
		if(error != 0)
			*error = (int)rxpacket[ERRBIT];
	}

	return result;
}

int CM730::MakeWord(int lowbyte, int highbyte)
{
	unsigned short word;

	word = highbyte;
	word = word << 8;
	word = word + lowbyte;

	return (int)word;
}

int CM730::GetLowByte(int word)
{
	unsigned short temp;
	temp = word & 0xff;
	return (int)temp;
}

int CM730::GetHighByte(int word)
{
	unsigned short temp;
	temp = word & 0xff00;
	return (int)(temp >> 8);
}

int CM730::MakeColor(int red, int green, int blue)
{
	int r = red & 0xFF;
	int g = green & 0xFF;
	int b = blue & 0xFF;

	return (int)(((b>>3)<<10)|((g>>3)<<5)|(r>>3));
}

// ***   WEBOTS PART  *** //
//
void CM730::MakeBulkReadPacket()
{
	int number = 0;

	m_BulkReadTxPacket[ID]              = (unsigned char)ID_BROADCAST;
	m_BulkReadTxPacket[INSTRUCTION]     = INST_BULK_READ;
	m_BulkReadTxPacket[PARAMETER]       = (unsigned char)0x0;

	if(Ping(CM730::ID_CM, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 30;
		m_BulkReadTxPacket[PARAMETER+3*number+2] = CM730::ID_CM;
		m_BulkReadTxPacket[PARAMETER+3*number+3] = CM730::P_DXL_POWER;
		number++;
	}

	//    for(int id = 1; id < JointData::NUMBER_OF_JOINTS; id++)
	//    {
	//        if(MotionStatus::m_CurrentJoints.GetEnable(id))
	//        {
	//            m_BulkReadTxPacket[PARAMETER+3*number+1] = 2;   // length
	//            m_BulkReadTxPacket[PARAMETER+3*number+2] = id;  // id
	//            m_BulkReadTxPacket[PARAMETER+3*number+3] = MX28::P_PRESENT_POSITION_L; // start address
	//            number++;
	//        }
	//    }

	if(Ping(FSR::ID_L_FSR, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 10;               // length
		m_BulkReadTxPacket[PARAMETER+3*number+2] = FSR::ID_L_FSR;   // id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = FSR::P_FSR1_L;    // start address
		number++;
	}

	if(Ping(FSR::ID_R_FSR, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 10;               // length
		m_BulkReadTxPacket[PARAMETER+3*number+2] = FSR::ID_R_FSR;   // id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = FSR::P_FSR1_L;    // start address
		number++;
	}

	m_BulkReadTxPacket[LENGTH]          = (number * 3) + 3;
}

void CM730::MakeBulkReadPacketWb()
{
	int number = 0;

	m_BulkReadTxPacket[ID] = (unsigned char)ID_BROADCAST;
	m_BulkReadTxPacket[INSTRUCTION] = INST_BULK_READ;
	m_BulkReadTxPacket[PARAMETER] = (unsigned char)0x0;

	if(Ping(CM730::ID_CM, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 30;
		m_BulkReadTxPacket[PARAMETER+3*number+2] = CM730::ID_CM;
		m_BulkReadTxPacket[PARAMETER+3*number+3] = CM730::P_DXL_POWER;
		number++;
	}

	for(int id = 1; id < JointData::NUMBER_OF_JOINTS; id++)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 8; // length (limits + goal + speed + torque)
		m_BulkReadTxPacket[PARAMETER+3*number+2] = id;	// id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = MX28::P_TORQUE_LIMIT_L; // start address
		number++;
	}

	m_BulkReadTxPacket[LENGTH] = (number * 3) + 3;
}

void CM730::MakeBulkReadPacketMPC()
{
	int number = 0;

	m_BulkReadTxPacket[ID] = (unsigned char)ID_BROADCAST;
	m_BulkReadTxPacket[INSTRUCTION] = INST_BULK_READ;
	m_BulkReadTxPacket[PARAMETER] = (unsigned char)0x0;

	if(Ping(CM730::ID_CM, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 30; // 1
		//2 m_BulkReadTxPacket[PARAMETER+3*number+1] = 12; // 2
		m_BulkReadTxPacket[PARAMETER+3*number+2] = CM730::ID_CM;
		m_BulkReadTxPacket[PARAMETER+3*number+3] = CM730::P_DXL_POWER; // 1
		//2 m_BulkReadTxPacket[PARAMETER+3*number+3] = CM730::P_GYRO_Z_L; // 2
		number++;
	}
	else {
		printf("Couldn't reach CM730 and making of bulkread packet!\n");
	}

	// length (limits + goal + speed + torque)x2 + (voltage + temp)x1
	//const int MPC_READ_LENGTH = 8;
	// length (goal + speed)x2
	const int MPC_READ_LENGTH = 4;

	for(int id = 1; id < JointData::NUMBER_OF_JOINTS; id++)
	{
		if(MotionStatus::m_CurrentJoints.GetEnable(id))
		{
			m_BulkReadTxPacket[PARAMETER+3*number+1] = MPC_READ_LENGTH;
			m_BulkReadTxPacket[PARAMETER+3*number+2] = id;	// id
			//m_BulkReadTxPacket[PARAMETER+3*number+3] = MX28::P_TORQUE_LIMIT_L; // start address
			m_BulkReadTxPacket[PARAMETER+3*number+3] = MX28::P_PRESENT_POSITION_L; // start address
			number++;
		}
	}

	if(Ping(FSR::ID_L_FSR, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 10;               // length
		m_BulkReadTxPacket[PARAMETER+3*number+2] = FSR::ID_L_FSR;   // id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = FSR::P_FSR1_L;    // start address
		number++;
	}

	if(Ping(FSR::ID_R_FSR, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 10;               // length
		m_BulkReadTxPacket[PARAMETER+3*number+2] = FSR::ID_R_FSR;   // id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = FSR::P_FSR1_L;    // start address
		number++;
	}

	m_BulkReadTxPacket[LENGTH] = (number * 3) + 3;
	printf("MPC BulkRead Packet == %d\n", number*3 + 3);

	// 20*3 + 3 = 90
}

void CM730::MakeBulkReadPacketServo25()
{
	int number = 0;

	m_BulkReadTxPacket[ID] = (unsigned char)ID_BROADCAST;
	m_BulkReadTxPacket[INSTRUCTION] = INST_BULK_READ;
	m_BulkReadTxPacket[PARAMETER] = (unsigned char)0x0;

	if(Ping(CM730::ID_CM, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 30;
		m_BulkReadTxPacket[PARAMETER+3*number+2] = CM730::ID_CM;
		m_BulkReadTxPacket[PARAMETER+3*number+3] = CM730::P_DXL_POWER;
		//m_BulkReadTxPacket[PARAMETER+3*number+3] = CM730::P_GYRO_Z_L;
		number++;
	}

	// length (limits + goal + speed + torque)x2 + (voltage + temp)x1
	const int MPC_READ_LENGTH = 10;
	//const int MPC_READ_LENGTH = 2;

	if(MotionStatus::m_CurrentJoints.GetEnable(CM730::ID_TEST_SERVO))
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = MPC_READ_LENGTH;
		m_BulkReadTxPacket[PARAMETER+3*number+2] = CM730::ID_TEST_SERVO;	// id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = MX28::P_TORQUE_LIMIT_L; // start address
		//m_BulkReadTxPacket[PARAMETER+3*number+3] = MX28::P_PRESENT_POSITION_L; // start address
		number++;
	}

	if(Ping(FSR::ID_L_FSR, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 10;               // length
		m_BulkReadTxPacket[PARAMETER+3*number+2] = FSR::ID_L_FSR;   // id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = FSR::P_FSR1_L;    // start address
		number++;
	}

	if(Ping(FSR::ID_R_FSR, 0) == SUCCESS)
	{
		m_BulkReadTxPacket[PARAMETER+3*number+1] = 10;               // length
		m_BulkReadTxPacket[PARAMETER+3*number+2] = FSR::ID_R_FSR;   // id
		m_BulkReadTxPacket[PARAMETER+3*number+3] = FSR::P_FSR1_L;    // start address
		number++;
	}

	m_BulkReadTxPacket[LENGTH] = (number * 3) + 3;
	printf("Single Servo BulkRead Packet == %d\n", number*3 + 3);

	// 20*3 + 3 = 90
}


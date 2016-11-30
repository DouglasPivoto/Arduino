#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <StreamString.h>
#include <base64.h>
#include <Hash.h>
#include "ESP8266MQTTClient.h"
#include "MQTTTransport.h"

MQTTTransportTraits::~MQTTTransportTraits()
{
}

std::unique_ptr<WiFiClient> MQTTTransportTraits::create()
{
	return std::unique_ptr<WiFiClient>(new WiFiClient());
}

bool MQTTTransportTraits::verify(WiFiClient& client, const char* host)
{
	return true;
}
bool MQTTTransportTraits::connect(WiFiClient& client, const char* host, int port)
{
	return client.connect(host, port);
}
int MQTTTransportTraits::write(WiFiClient& client, unsigned char *data, int size)
{
	return client.write(data, size);
}
int MQTTTransportTraits::read(WiFiClient& client, unsigned char *data, int size)
{
	return client.read(data, size);
}
/**
 * MQTT Over TLS
 */
MQTTTLSTraits::MQTTTLSTraits(const String& fingerprint) :
	_fingerprint(fingerprint)
{
}

std::unique_ptr<WiFiClient> MQTTTLSTraits::create()
{
	return std::unique_ptr<WiFiClient>(new WiFiClientSecure());
}

bool MQTTTLSTraits::verify(WiFiClient& client, const char* host)
{
	auto wcs = reinterpret_cast<WiFiClientSecure&>(client);
	return wcs.verify(_fingerprint.c_str(), host);
}
bool MQTTTLSTraits::connect(WiFiClient& client, const char* host, int port)
{
	auto wcs = reinterpret_cast<WiFiClientSecure&>(client);
	return wcs.connect(host, port);
}
int MQTTTLSTraits::write(WiFiClient& client, unsigned char *data, int size)
{
	auto wcs = reinterpret_cast<WiFiClientSecure&>(client);
	return wcs.write(data, size);
}
int MQTTTLSTraits::read(WiFiClient& client, unsigned char *data, int size)
{
	auto wcs = reinterpret_cast<WiFiClientSecure&>(client);
	return wcs.read(data, size);
}
/**
 * MQTT Over WS
 */

MQTTWSTraits::MQTTWSTraits()
{
	randomSeed(RANDOM_REG32);
}

std::unique_ptr<WiFiClient> MQTTWSTraits::create()
{
	return std::unique_ptr<WiFiClient>(new WiFiClient());
}

bool MQTTWSTraits::verify(WiFiClient& client, const char* host)
{
	return true;
}
bool MQTTWSTraits::connect(WiFiClient& client, const char* host, int port)
{
	uint8_t randomKey[16] = { 0 }, timeout = 0;
	int bite;
	bool foundupgrade = false;
	String serverKey, temp, acceptKey;

	for(uint8_t i = 0; i < sizeof(randomKey); i++) {
		randomKey[i] = random(0xFF);
	}
	_key = base64::encode(randomKey, 16);
	LOG("Key: %s\r\n", _key.c_str());
	String handshake = "GET / HTTP/1.1\r\n"
	                   "Connection: Upgrade\r\n"
	                   "Upgrade: websocket\r\n"
	                   "Host: " + String(host) + "\r\n"
	                   "Sec-WebSocket-Version: 13\r\n"
	                   "Sec-WebSocket-Key: " + _key + "\r\n\r\n";
	if(!client.connect(host, port))
		return false;
	client.write(handshake.c_str(), handshake.length());

	while(client.connected() && !client.available()) {
		delay(100);
		if(timeout++ > 10)
			return false;
	}
	while((bite = client.read()) != -1) {

		temp += (char)bite;

		if((char)bite == '\n') {
			if(!foundupgrade && temp.startsWith("Upgrade: websocket")) {
				foundupgrade = true;
			} else if(temp.startsWith("Sec-WebSocket-Accept: ")) {
				serverKey = temp.substring(22, temp.length() - 2); // Don't save last CR+LF
			}
			temp = "";
		}

		if(!client.available()) {
			delay(100);
		}
	}
	_key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	uint8_t sha1HashBin[20] = { 0 };
	sha1(_key, &sha1HashBin[0]);
	acceptKey = base64::encode(sha1HashBin, 20);
	acceptKey.trim();
	LOG("AccpetKey: %s\r\n", acceptKey.c_str());
	LOG("ServerKey: %s\r\n", serverKey.c_str());
	timeout = 0;
	return acceptKey == serverKey;
}

int MQTTWSTraits::write(WiFiClient& client, unsigned char *data, int size)
{
	char header_len = 0, *mask, *data_buffer;
	int written = 0;
	data_buffer = (char *) malloc(8 + size);
	if(data_buffer == NULL)
		return -1;
	// Opcode; final fragment
	data_buffer[header_len++] = WS_OPCODE_BINARY | WS_FIN;

	// NOTE: no support for > 16-bit sized messages
	if(size > 125) {
		data_buffer[header_len++] = WS_SIZE16 | WS_MASK;
		data_buffer[header_len++] = (uint8_t)(size >> 8);
		data_buffer[header_len++] = (uint8_t)(size & 0xFF);
	} else {
		data_buffer[header_len++] = (uint8_t)(size | WS_MASK);
	}
	mask = &data_buffer[header_len];
	data_buffer[header_len++] = random(0, 256);
	data_buffer[header_len++] = random(0, 256);
	data_buffer[header_len++] = random(0, 256);
	data_buffer[header_len++] = random(0, 256);

	for(int i=0; i<size; i++) {
		LOG("0x%X ", data[i]);
	}
	LOG("\r\n---\r\n");
	for(int i = 0; i < size; ++i) {
		data_buffer[header_len++] = (data[i] ^ mask[i % 4]);
	}
	for(int i=0; i<header_len; i++) {
		LOG("0x%X ", data_buffer[i]);
	}
	LOG("\r\n");
	client.write(data_buffer, header_len);
	free(data_buffer);
	return size;
}
int MQTTWSTraits::read(WiFiClient& client, unsigned char *data, int size)
{
	unsigned char *data_buffer = (unsigned char*) malloc(size + 10), *data_ptr, opcode, mask, *maskKey = NULL;
	int tcp_read_size, payloadLen;
	data_ptr = data_buffer;
	if(data_buffer == NULL)
		return -1;

	tcp_read_size = client.read(data_buffer, size + 10);

	if(tcp_read_size <= 0)
	{
		free(data_buffer);
		return -1;
	}
	opcode = (*data_ptr & 0x0F);
	data_ptr ++;
	mask = ((*data_ptr >> 7) & 0x01);
	data_ptr ++;
	payloadLen = (*data_ptr & 0x7F);
	data_ptr++;
	if(payloadLen == 126) {
		// headerLen += 2;
		payloadLen = data_ptr[0] << 8 | data_ptr[1];
		data_ptr += 2;
	} else if(payloadLen == 127) {
		// headerLen += 8;

		if(data_ptr[0] != 0 || data_ptr[1] != 0 || data_ptr[2] != 0 || data_ptr[3] != 0) {
			// really too big!
			payloadLen = 0xFFFFFFFF;
		} else {
			payloadLen = data_ptr[4] << 24 | data_ptr[5] << 16 | data_ptr[6] << 8 | data_ptr[7];
		}
		data_ptr += 8;
	}

	if(mask) {
		maskKey = data_ptr;
		data_ptr += 4;
		for(size_t i = 0; i < payloadLen; i++) {
			data[i] = (data_ptr[i] ^ maskKey[i % 4]);
		}
	}
	return payloadLen;
}
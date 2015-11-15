/*
 * Copyright (c) 2014, 2015 Ahmed Samy  <f.fallen45@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "peer.h"
#include "torrent.h"

#include <iostream>

Peer::Peer(Torrent *torrent)
	: m_torrent(torrent),
	  m_conn(new Connection())
{
	m_state = PS_AmChoked | PS_PeerChoked;
}

Peer::Peer(const ConnectionPtr &c, Torrent *t)
	: m_torrent(t),
	  m_conn(c)
{
	m_state = PS_AmChoked | PS_PeerChoked;
}

Peer::~Peer()
{
	for (Piece *p : m_queue)
		delete p;
	m_queue.clear();
}

void Peer::disconnect()
{
	m_conn->close(false);
	m_conn->setErrorCallback(nullptr);	// deref
}

void Peer::connect(const std::string &ip, const std::string &port)
{
	m_conn->setErrorCallback(std::bind(&Peer::handleError, shared_from_this(), std::placeholders::_1));
	m_conn->connect(ip, port,
		[this] ()
		{
			const uint8_t *m_handshake = m_torrent->handshake();
			m_conn->write(m_handshake, 68);
			m_conn->read(68,
				[this, m_handshake] (const uint8_t *handshake, size_t size)
				{
					if (size != 68
						|| (handshake[0] != 0x13 && memcmp(&handshake[1], "BitTorrent protocol", 19) != 0)
						|| memcmp(&handshake[28], &m_handshake[28], 20) != 0)
						return handleError("info hash/protocol type mismatch");

					std::string peerId((const char *)&handshake[48], 20);
					if (!m_peerId.empty() && peerId != m_peerId)
						return handleError("peer id mismatch: unverified");

					m_peerId = peerId;
					m_torrent->addPeer(shared_from_this());
					m_torrent->sendBitfield(shared_from_this());
					m_conn->read(4, std::bind(&Peer::handle, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
				}
			);
		}
	);
}

void Peer::verify()
{
	const uint8_t *m_handshake = m_torrent->handshake();
	m_conn->setErrorCallback(std::bind(&Peer::handleError, shared_from_this(), std::placeholders::_1));
	m_conn->read(68,
		[this, m_handshake] (const uint8_t *handshake, size_t size)
		{
			if (size != 68
				|| (handshake[0] != 0x13 && memcmp(&handshake[1], "BitTorrent protocol", 19) != 0)
				|| memcmp(&handshake[28], &m_handshake[28], 20) != 0)
				return handleError("info hash/protocol type mismatch");

			std::string peerId((const char *)&handshake[48], 20);
			if (!m_peerId.empty() && peerId != m_peerId)
				return handleError("unverified");

			m_peerId = peerId;
			m_conn->write(m_handshake, 68);
			m_torrent->handleNewPeer(shared_from_this());
			m_conn->read(4, std::bind(&Peer::handle, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
		}
	);
}

void Peer::handle(const uint8_t *data, size_t size)
{
	if (size != 4)
		return handleError("Peer::handle(): Expected 4-byte length");

	uint32_t length = readBE32(data);
	switch (length) {
	case 0: // Keep alive
		return m_conn->read(4, std::bind(&Peer::handle, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
	default:
		m_conn->read(length,
			[this] (const uint8_t *data, size_t size)
			{
				InputMessage in(const_cast<uint8_t *>(&data[1]), size - 1, ByteOrder::BigEndian);
				handleMessage((MessageType)data[0], in);
			}
		);
	}
}

void Peer::handleMessage(MessageType messageType, InputMessage in)
{
	size_t payloadSize = in.getSize();

	switch (messageType) {
	case MT_Choke:
		if (payloadSize != 0)
			return handleError("invalid choke-message size");

		m_torrent->handlePeerDebug(shared_from_this(), "choke");
		m_state |= PS_PeerChoked;
		break;
	case MT_UnChoke:
		if (payloadSize != 0)
			return handleError("invalid unchoke-message size");

		m_state &= ~PS_PeerChoked;		
		m_torrent->handlePeerDebug(shared_from_this(), "unchoke");

		for (Piece *piece : m_queue)
			requestPiece(piece->index);
		break;
	case MT_Interested:
	{
		if (payloadSize != 0)
			return handleError("invalid interested-message size");

		m_torrent->handlePeerDebug(shared_from_this(), "interested");
		m_state |= PS_PeerInterested;

		if (isLocalChoked()) {
			// 4-byte length, 1-byte packet type
			static const uint8_t unchoke[5] = { 0, 0, 0, 1, MT_UnChoke };
			m_conn->write(unchoke, sizeof(unchoke));
			m_state &= ~PS_AmChoked;
		}

		break;
	}
	case MT_NotInterested:
		if (payloadSize != 0)
			return handleError("invalid not-interested-message size");

		m_torrent->handlePeerDebug(shared_from_this(), "not interested");
		m_state &= ~PS_PeerInterested;
		break;
	case MT_Have:
	{
		if (payloadSize != 4)
			return handleError("invalid have-message size");

		uint32_t p = in.getU32();
		if (!hasPiece(p))
			pushPiece(p);
		break;
	}
	case MT_Bitfield:
	{
		if (payloadSize < 1)
			return handleError("invalid bitfield-message size");

		m_torrent->handlePeerDebug(shared_from_this(), "bit field");
		uint8_t *buf = in.getBuffer();
#if 0	// FIXME: This is broken for bytes that start with 4 zero bits.
		for (size_t i = 0, index = 0; i < payloadSize; ++i) {
			uint8_t b = buf[i];
			if (b == 0) {
				index += 8;
				continue;
			}

			uint8_t leading = CHAR_BIT - (sizeof(unsigned int) * CHAR_BIT - __builtin_clz(b));
			uint8_t trailing = __builtin_ctz(b);

			// skip leading zero bits first, we skip trailing zero bits later
			index += leading;

			// push this piece, we know it's there
			pushPiece(index++);

			for (b >>= trailing + (leading | 1); b != 0; b >>= 1, ++index)
				if (b & 1)
					pushPiece(index);

			// skip trailing
			index += trailing;
		}
#else
		for (size_t i = 0, index = 0; i < payloadSize; ++i) {
			for (uint8_t x = 128; x > 0; x >>= 1) {
				if ((buf[i] & x) != x) {
					++index;
					continue;
				}

				if (index >= m_torrent->totalPieces())
					break;

				pushPiece(index++);
			}
		}
#endif

		if (!m_torrent->isFinished())
			m_torrent->requestPiece(shared_from_this());
		break;
	}
	case MT_Request:
	{
		if (payloadSize != 12)
			return handleError("invalid request-message size");

		if (!isRemoteInterested())
			return handleError("peer requested piece block without showing interest");

		if (isLocalChoked())
			return handleError("peer requested piece while choked");

		uint32_t index, begin, length;
		in >> index;
		in >> begin;
		in >> length;

		if (length > maxRequestSize)
			return handleError("peer requested piece of size " + bytesToHumanReadable(length, true) + " which is beyond our max request size");

		m_torrent->handlePeerDebug(shared_from_this(), "requested piece block of length " + bytesToHumanReadable(length, true));
		m_torrent->handleRequestBlock(shared_from_this(), index, begin, length);
		break;
	}
	case MT_PieceBlock:
	{
		if (payloadSize < 9)
			return handleError("invalid pieceblock-message size");

		uint32_t index, begin;
		in >> index;
		in >> begin;

		payloadSize -= 8;	// deduct index and begin
		if (payloadSize == 0 || payloadSize > maxRequestSize)
			return handleError("received too big piece block of size " + bytesToHumanReadable(payloadSize, true));

		auto it = std::find_if(m_queue.begin(), m_queue.end(),
				[index](const Piece *piece) { return piece->index == index; });
		if (it == m_queue.end())
			return handleError("received piece " + std::to_string(index) + " which we did not ask for");

		Piece *piece = *it;
		uint32_t blockIndex = begin / maxRequestSize;
		if (blockIndex >= piece->numBlocks)
			return handleError("received too big block index");

		if (m_torrent->pieceDone(index)) {
			m_torrent->handlePeerDebug(shared_from_this(), "cancelling " + std::to_string(index));
			sendCancelRequest(piece);
			m_queue.erase(it);
			delete piece;
		} else {
			piece->blocks[blockIndex].size = payloadSize;
			piece->blocks[blockIndex].data = in.getBuffer(payloadSize);

			if (++piece->currentBlocks == piece->numBlocks) {
				DataBuffer<uint8_t> pieceData;
				pieceData.reserve(piece->numBlocks * maxRequestSize);	// just a prediction could be bit less
				for (size_t x = 0; x < piece->numBlocks; ++x)
					for (size_t y = 0; y < piece->blocks[x].size; ++y)
						pieceData.add_unchecked(piece->blocks[x].data[y]);

				bool cont = m_torrent->handlePieceCompleted(shared_from_this(), index, pieceData);
				m_queue.erase(it);
				delete piece;

				if (cont) {
					// We have to do this here, if we do it inside of handlePieceCompleted
					// m_queue will fail us due to sendPieceRequest changing position
					if (m_torrent->completedPieces() != m_torrent->totalPieces())
						m_torrent->requestPiece(shared_from_this());
				} else {
					// sending malicious pieces?
					return disconnect();
				}
			}
		}

		break;
	}
	case MT_Cancel:
	{
		if (payloadSize != 12)
			return handleError("invalid cancel-message size");

		uint32_t index, begin, length;
		in >> index;
		in >> begin;
		in >> length;

		m_torrent->handlePeerDebug(shared_from_this(), "cancel");
		break;
	}
	case MT_Port:
		if (payloadSize != 2)
			return handleError("invalid port-message size");

		uint16_t port;
		in >> port;
		break;
	}

	m_conn->read(4, std::bind(&Peer::handle, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void Peer::handleError(const std::string &errmsg)
{
	m_torrent->removePeer(shared_from_this(), errmsg);
	disconnect();
}

void Peer::sendBitfield(const std::vector<uint8_t> &payload)
{
	OutputMessage out(ByteOrder::BigEndian, 5 + payload.size());
	out << 1UL + payload.size();
	out << (uint8_t)MT_Bitfield;
	out.addBytes(&payload[0], payload.size());

	m_conn->write(out);
}

void Peer::sendHave(uint32_t index)
{
	if (isLocalChoked())
		return;

	OutputMessage out(ByteOrder::BigEndian, 9);
	out << 5UL;		// length
	out << (uint8_t)MT_Have;
	out << index;

	m_conn->write(out);
}

void Peer::sendPieceBlock(uint32_t index, uint32_t begin, uint8_t *block, uint32_t length)
{
	OutputMessage out(ByteOrder::BigEndian, 13 + length);
	out << 9UL + length;	// length
	out << (uint8_t)MT_PieceBlock;
	out << index;
	out << begin;
	out.addBytes(block, length);

	m_conn->write(out);
}

void Peer::sendPieceRequest(uint32_t index)
{
	sendInterested();

	uint32_t pieceLength = m_torrent->pieceSize(index);
	size_t numBlocks = (int)(ceil(double(pieceLength) / maxRequestSize));

	Piece *piece = new Piece();
	piece->index = index;
	piece->currentBlocks = 0;
	piece->numBlocks = numBlocks;
	piece->blocks = new PieceBlock[numBlocks];

	m_queue.push_back(piece);
	if (!isRemoteChoked())
		requestPiece(index);
}

void Peer::sendRequest(uint32_t index, uint32_t begin, uint32_t length)
{
	OutputMessage out(ByteOrder::BigEndian, 17);
	out << 13UL;		// length
	out << (uint8_t)MT_Request;
	out << index;
	out << begin;
	out << length;

	m_conn->write(out);
}

void Peer::sendInterested()
{
	// 4-byte length, 1 byte packet type
	const uint8_t interested[5] = { 0, 0, 0, 1, MT_Interested };
	m_conn->write(interested, sizeof(interested));
	m_state |= PS_AmInterested;
}

void Peer::sendCancelRequest(Piece *p)
{
	size_t begin = 0;
	size_t length = m_torrent->pieceSize(p->index);
	for (; length > maxRequestSize; length -= maxRequestSize, begin += maxRequestSize)
		sendCancel(p->index, begin, maxRequestSize);
	sendCancel(p->index, begin, length);
}

void Peer::sendCancel(uint32_t index, uint32_t begin, uint32_t length)
{
	OutputMessage out(ByteOrder::BigEndian, 17);
	out << 13UL;	// length
	out << (uint8_t)MT_Cancel;
	out << index;
	out << begin;
	out << length;

	m_conn->write(out);
}

void Peer::requestPiece(size_t pieceIndex)
{
	if (isRemoteChoked())
		return handleError("Attempt to request piece from a peer that is remotely choked");

	size_t begin = 0;
	size_t length = m_torrent->pieceSize(pieceIndex);
	for (; length > maxRequestSize; length -= maxRequestSize, begin += maxRequestSize)
		sendRequest(pieceIndex, begin, maxRequestSize);
	sendRequest(pieceIndex, begin, length);
}


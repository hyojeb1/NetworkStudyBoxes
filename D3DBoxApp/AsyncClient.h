#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <queue>

using boost::asio::ip::tcp;

struct MoveTarget
{
    int x;
    int z;
};


class AsyncClient : public std::enable_shared_from_this<AsyncClient>
{
public:
    AsyncClient(boost::asio::io_context& io,
        const std::string& host,
        uint16_t port)
        : m_IO(io)
        , m_Socket(io)
        , m_Endpoint(boost::asio::ip::make_address(host), port)
    {
    }

    void Start()
    {
        auto self = shared_from_this();
        m_Socket.async_connect(m_Endpoint,
            [this, self](boost::system::error_code ec)
            {
                if (!ec)
                    DoRead();
            });
    }

    void Send(const std::string& msg)
    {
        auto self = shared_from_this();
        boost::asio::post(m_IO,
            [this, self, msg]()
            {
                bool writing = !m_WriteQueue.empty();
                m_WriteQueue.push_back(msg);
                if (!writing)
                    DoWrite();
            });
    }

    bool PopMoveTarget(MoveTarget& out)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_TargetQueue.empty())
            return false;

        out = m_TargetQueue.front();
        m_TargetQueue.pop();
        return true;
    }

    bool PopLine(std::string& out)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Lines.empty()) return false;
        out = m_Lines.front();
        m_Lines.pop();
        return true;
    }


private:
    void DoRead()
    {
        auto self = shared_from_this();
        m_Socket.async_read_some(
            boost::asio::buffer(m_ReadBuf),
            [this, self](boost::system::error_code ec, std::size_t len)
            {
                if (!ec)
                {
                    Parse(m_ReadBuf.data(), len);
                    DoRead();
                }
            });
    }

    void DoWrite()
    {
        auto self = shared_from_this();
        boost::asio::async_write(
            m_Socket,
            boost::asio::buffer(m_WriteQueue.front()),
            [this, self](boost::system::error_code ec, std::size_t)
            {
                if (!ec)
                {
                    m_WriteQueue.pop_front();
                    if (!m_WriteQueue.empty())
                        DoWrite();
                }
            });

    }

    void EnqueueLine(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Lines.push(line);
    }

    void Parse(const char* data, size_t len)
    {
        m_Incoming.append(data, len);

        size_t pos = 0;
        while ((pos = m_Incoming.find('\n')) != std::string::npos)
        {
            std::string line = m_Incoming.substr(0, pos);
            m_Incoming.erase(0, pos + 1);

            EnqueueLine(line); // 스레드 안전 큐에 push
        }
    }


private:
    boost::asio::io_context& m_IO;
    tcp::socket m_Socket;
    tcp::endpoint m_Endpoint;

    std::array<char, 512> m_ReadBuf{};
    std::deque<std::string> m_WriteQueue;

    std::string m_Incoming;
    std::mutex m_Mutex;
    std::queue<std::string> m_Lines;
    std::queue<MoveTarget> m_TargetQueue;
};



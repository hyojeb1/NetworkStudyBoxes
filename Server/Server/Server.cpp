#include <boost/asio.hpp>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <deque>
#include <memory>
#include <sstream>
#include <array>

using boost::asio::ip::tcp;

// =====================================================
// World State
// =====================================================
struct Block
{
    int key; // sessionKey
    int x;
    int z;
};

std::unordered_map<int, Block> g_Blocks;
int g_NextSessionKey = 1;

// forward
class Session;
std::vector<std::shared_ptr<Session>> g_Sessions;

void Broadcast(const std::string& msg);

// =====================================================
// Session
// =====================================================
class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket socket)
        : m_Socket(std::move(socket))
        , m_SessionKey(g_NextSessionKey++)
    {
    }

    void Start()
    {
        // 1. 세션 키 할당
        Send("ASSIGN " + std::to_string(m_SessionKey) + "\n");

        // 2. 현재 월드 스냅샷 전송
        SendSnapshot();

        DoRead();
    }

    void Send(const std::string& msg)
    {
        auto self = shared_from_this();
        boost::asio::post(
            m_Socket.get_executor(),
            [this, self, msg]()
            {
                bool writing = !m_WriteQueue.empty();
                m_WriteQueue.push_back(msg);
                if (!writing)
                    DoWrite();
            });
    }

private:
    // -------------------------
    // Snapshot
    // -------------------------
    void SendSnapshot()
    {
        Send("SNAPSHOT_BEGIN\n");

        for (auto& [key, b] : g_Blocks)
        {
            Send(
                "SPAWN " +
                std::to_string(b.key) + " " +
                std::to_string(b.x) + " " +
                std::to_string(b.z) + "\n");
        }

        Send("SNAPSHOT_END\n");
    }

    // -------------------------
    // Read
    // -------------------------
    void DoRead()
    {
        auto self = shared_from_this();
        m_Socket.async_read_some(
            boost::asio::buffer(m_ReadBuf),
            [this, self](boost::system::error_code ec, std::size_t len)
            {
                if (ec)
                {

                    //여기서 ec를 이용해서 string을 개조하는 거임. 어떤데
                    //응~ 소멸자에서 부르는 게 더 깔끔해~
                    std::cout << "[DISCONNECT] sessionKey=" << m_SessionKey << "\n";

                    //
                    auto it = g_Blocks.find(m_SessionKey);

                    //std::cout << "g_Blocks.size(): " << g_Blocks.size() << std::endl;

                    if (it != g_Blocks.end())
                    {
                        //
                        std::string despawnMsg =
                            "DESPAWN " +
                            std::to_string(m_SessionKey) + "\n";
                            //std::to_string(m_SessionKey) + " " +
                            //std::to_string(it->second.x) + " " +
                            //std::to_string(it->second.z) + "\n";
                        
                        Broadcast(despawnMsg);

                        g_Blocks.erase(it);
                    }

                    auto sessionIt = std::find(g_Sessions.begin(), g_Sessions.end(), self);
                    if (sessionIt != g_Sessions.end())
                    {
                        g_Sessions.erase(sessionIt);
                    }
                    
                    return;
                }

                m_Incoming.append(m_ReadBuf.data(), len);

                std::size_t pos;
                while ((pos = m_Incoming.find('\n')) != std::string::npos)
                {
                    std::string line = m_Incoming.substr(0, pos);
                    m_Incoming.erase(0, pos + 1);
                    HandleCommand(line);
                }

                DoRead();
            });
    }

    // -------------------------
    // Command handling
    // -------------------------
    void HandleCommand(const std::string& line)
    {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // =========================
        // SPAWN <cellX> <cellZ>
        // =========================
        if (cmd == "SPAWN")
        {
            int x, z;
            iss >> x >> z;

            // 세션당 1회만 허용
            if (g_Blocks.find(m_SessionKey) != g_Blocks.end())
                return;

            Block b;
            b.key = m_SessionKey;
            b.x = x;
            b.z = z;

            g_Blocks[m_SessionKey] = b;

            std::cout << "[SPAWN] key=" << m_SessionKey
                << " (" << x << "," << z << ")\n";

            Broadcast(
                "SPAWN " +
                std::to_string(b.key) + " " +
                std::to_string(b.x) + " " +
                std::to_string(b.z) + "\n");
        }

        // =========================
        // MOVE <cellX> <cellZ>
        // =========================
        else if (cmd == "MOVE")
        {
            int x, z;
            iss >> x >> z;

            auto it = g_Blocks.find(m_SessionKey);
            if (it == g_Blocks.end())
                return; // 아직 SPAWN 안 됨

            it->second.x = x;
            it->second.z = z;

            std::cout << "[MOVE] key=" << m_SessionKey
                << " (" << x << "," << z << ")\n";

            Broadcast(
                "MOVE " +
                std::to_string(m_SessionKey) + " " +
                std::to_string(x) + " " +
                std::to_string(z) + "\n");
        }

        // =========================
        // DESPAWN <cellX> <cellZ>
        // 끊어지는 코드는 서버에서 인식을 하는 게 맞는 것 같은데...
        // =========================
        else if (cmd == "DESPAWN")
        {
            auto it = g_Blocks.find(m_SessionKey);
            if (it == g_Blocks.end())
                return;

            std::cout << "[DESPAWN] key=" << m_SessionKey << "\n";

            Broadcast(
                "DESPAWN " +
                std::to_string(m_SessionKey) + "\n");
        }



    }

    // -------------------------
    // Write
    // -------------------------
    void DoWrite()
    {
        auto self = shared_from_this();
        boost::asio::async_write(
            m_Socket,
            boost::asio::buffer(m_WriteQueue.front()),
            [this, self](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                    return;

                m_WriteQueue.pop_front();
                if (!m_WriteQueue.empty())
                    DoWrite();
            });
    }

    const int getSessionKey() const { return m_SessionKey; };

private:
    tcp::socket m_Socket;
    int m_SessionKey;

    std::array<char, 1024> m_ReadBuf;
    std::string m_Incoming;
    std::deque<std::string> m_WriteQueue;
};

// =====================================================
// Broadcast helper
// =====================================================
void Broadcast(const std::string& msg)
{
    for (auto& s : g_Sessions)
        s->Send(msg);
}

// =====================================================
// Accept loop
// =====================================================
void DoAccept(tcp::acceptor& acceptor)
{
    acceptor.async_accept(
        [&](boost::system::error_code ec, tcp::socket socket)
        {
            if (!ec)
            {
                // g_NextSessionKey이 m_SessionKey인진 잘 모르겠다. 많은 세션중에 일부인데.
                std::cout << "[CONNECT] sessionKey=" << g_NextSessionKey << "\n";
                auto session = std::make_shared<Session>(std::move(socket));
                g_Sessions.push_back(session);
                session->Start();
            }
            DoAccept(acceptor);
        });
}

// =====================================================
// main
// =====================================================
int main()
{
    try
    {
        boost::asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 8080));

        std::cout << "Server started on port 8080\n";
        DoAccept(acceptor);

        io.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Server exception: " << e.what() << "\n";
    }

    return 0;
}

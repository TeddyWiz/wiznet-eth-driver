
/**
  ******************************************************************************
  * @file    W5500Interface.h
  * @author  Bongjun Hur (modified version from Sergei G (https://os.mbed.com/users/sgnezdov/))
  * @brief   Implementation file of the NetworkStack for the W5500 Device
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, WIZnet SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2017 WIZnet Co.,Ltd.</center></h2>
  ******************************************************************************
  */

#include "mbed.h"
#include "W5500Interface.h"

static uint8_t W5500_DEFAULT_TESTMAC[6] = {0x00, 0x08, 0xdc, 0x19, 0x85, 0xa8};
static int udp_local_port = 0;

#define SKT(h) ((w5500_socket*)h)
#define w5500_WAIT_TIMEOUT   400
#define w5500_ACCEPT_TIMEOUT 300000 //5 mins timeout, retrun NSAPI_ERROR_WOULD_BLOCK if there is no connection during 5 mins

#define w5500_INTF_DBG 0

#if w5500_INTF_DBG
#define DBG(...) do{debug("[%s:%d]", __PRETTY_FUNCTION__,__LINE__);debug(__VA_ARGS__);} while(0);
#else
#define DBG(...) while(0);
#define INFO(...) do{debug("[%s:%d]", __PRETTY_FUNCTION__,__LINE__);debug(__VA_ARGS__);} while(0);
#endif

/**
 * @brief   Defines a custom MAC address
 * @note    Have to be unique within the connected network!
 *          Modify the mac array items as needed.
 * @param   mac A 6-byte array defining the MAC address
 * @retval
 */
/* Interface implementation */

W5500Interface::W5500Interface(PinName mosi, PinName miso, PinName sclk, PinName cs, PinName reset) :
    _w5500(mosi, miso, sclk, cs, reset)
{
    ip_set = false;
    _dhcp_enable = true;

//    _w5500.attach(this, &W5500Interface::event);
    thread_read_socket.start(callback(this, &W5500Interface::socket_check_read));
}

/*
W5500Interface::W5500Interface(SPI* spi, PinName cs, PinName reset) :
    _w5500(spi, cs, reset)
{
    ip_set = false;
}
*/

w5500_socket* W5500Interface::get_sock(int fd)
{
    for (int i=0; i<MAX_SOCK_NUM ; i++) {
        if (w5500_sockets[i].fd == -1) {
            w5500_sockets[i].fd            = fd;
            w5500_sockets[i].proto         = NSAPI_TCP;
            w5500_sockets[i].connected     = false;
            w5500_sockets[i].callback      = NULL;
            w5500_sockets[i].callback_data = NULL;
            return &w5500_sockets[i];
        }
    }
    return NULL;
}

void W5500Interface::init_socks()
{
    for (int i=0; i<MAX_SOCK_NUM ; i++) {
        w5500_sockets[i].fd            = -1;
        w5500_sockets[i].proto         = NSAPI_TCP;
        w5500_sockets[i].connected     = false;
        w5500_sockets[i].callback      = NULL;
        w5500_sockets[i].callback_data = NULL;
    }

    dns.setup(get_stack());
    //initialize the socket isr
    //_daemon = new Thread(osPriorityNormal, 1024);
    //_daemon->start(callback(this, &W5500Interface::daemon));
}

uint32_t W5500Interface:: str_to_ip(const char* str)
{
    uint32_t ip = 0;
    char* p = (char*)str;
    for(int i = 0; i < 4; i++) {
        ip |= atoi(p);
        p = strchr(p, '.');
        if (p == NULL) {
            break;
        }
        ip <<= 8;
        p++;
    }
    return ip;
}

int W5500Interface::init()
{
    _dhcp_enable = true;
    _w5500.reg_wr<uint32_t>(SIPR, 0x00000000); // local ip "0.0.0.0"
    //_w5500.reg_wr<uint8_t>(SIMR, 0xFF); //
    for (int i =0; i < 6; i++) _w5500.mac[i] = W5500_DEFAULT_TESTMAC[i];
    _w5500.setmac();
    _w5500.reset();
    init_socks();
    return 0;
}

int W5500Interface::init(uint8_t * mac)
{
    _dhcp_enable = true;
    _w5500.reg_wr<uint32_t>(SIPR, 0x00000000); // local ip "0.0.0.0"
    // should set the mac address and keep the value in this class
    for (int i =0; i < 6; i++) _w5500.mac[i] = mac[i];
    _w5500.setmac();
    _w5500.reset();  // reset chip and write mac address
    init_socks();
    return 0;
}

// add this function, because sometimes no needed MAC address in init calling.
int W5500Interface::init(const char* ip, const char* mask, const char* gateway)
{
    _dhcp_enable = false;
    
    _w5500.ip = str_to_ip(ip);
    strcpy(ip_string, ip);
    ip_set = true;
    _w5500.netmask = str_to_ip(mask);
    _w5500.gateway = str_to_ip(gateway);
    _w5500.reset();

    // @Jul. 8. 2014 add code. should be called to write chip.
    _w5500.setip();
    init_socks();

    return 0;
}

int W5500Interface::init(uint8_t * mac, const char* ip, const char* mask, const char* gateway)
{
    _dhcp_enable = false;
    //
    for (int i =0; i < 6; i++) _w5500.mac[i] = mac[i];
    //
    _w5500.ip = str_to_ip(ip);
    strcpy(ip_string, ip);
    ip_set = true;
    _w5500.netmask = str_to_ip(mask);
    _w5500.gateway = str_to_ip(gateway);
    _w5500.reset();

    // @Jul. 8. 2014 add code. should be called to write chip.
    _w5500.setmac();
    _w5500.setip();
    init_socks();

    return 0;
}


void W5500Interface::socket_check_read()
{
    while (1) {
        for (int i = 0; i < MAX_SOCK_NUM; i++) {
            _mutex.lock();
                for (int i=0; i<MAX_SOCK_NUM ; i++) {
                    if (w5500_sockets[i].fd >= 0 && w5500_sockets[i].callback) {
                    	int size = _w5500.sreg<uint16_t>(w5500_sockets[i].fd, Sn_RX_RSR);
                        if (size > 0) {
                            //led1 = !led1;
                            w5500_sockets[i].callback(w5500_sockets[i].callback_data);
                        }
                    }
            }
            _mutex.unlock();
        }
        wait_ms(1);
    }
}

int W5500Interface::IPrenew(int timeout_ms)
{
    DBG("[EasyConnect] DHCP start\n");
    int err = dhcp.setup(get_stack(), _w5500.mac, timeout_ms);
    if (err == (-1)) {
        DBG("[EasyConnect] Timeout.\n");
        return NSAPI_ERROR_DHCP_FAILURE;
    }
    DBG("[EasyConnect] DHCP completed\n");
    DBG("[EasyConnect] Connected, IP: %d.%d.%d.%d\r\n", dhcp.yiaddr[0], dhcp.yiaddr[1], dhcp.yiaddr[2], dhcp.yiaddr[3]);

    char ip[24], gateway[24], netmask[24], dnsaddr[24];
    sprintf(ip,      "%d.%d.%d.%d", dhcp.yiaddr[0],  dhcp.yiaddr[1],  dhcp.yiaddr[2],  dhcp.yiaddr[3]);
    sprintf(gateway, "%d.%d.%d.%d", dhcp.gateway[0], dhcp.gateway[1], dhcp.gateway[2], dhcp.gateway[3]);
    sprintf(netmask, "%d.%d.%d.%d", dhcp.netmask[0], dhcp.netmask[1], dhcp.netmask[2], dhcp.netmask[3]);
    sprintf(dnsaddr, "%d.%d.%d.%d", dhcp.dnsaddr[0], dhcp.dnsaddr[1], dhcp.dnsaddr[2], dhcp.dnsaddr[3]);

    init(ip, netmask, gateway);
    setDnsServerIP(dnsaddr);

    _dhcp_enable = true; // because this value was changed in init(ip, netmask, gateway).

    return 0;
}


int W5500Interface::connect()
{
    if (_dhcp_enable) {
        init(); // init default mac address
        int err = IPrenew(15000);
		if (err < 0) return err;
    }

    if (_w5500.setip() == false) return NSAPI_ERROR_DHCP_FAILURE;
    return 0;
}

bool W5500Interface::setDnsServerIP(const char* ip_address)
{
    return dns.set_server(ip_address);
}

int W5500Interface::disconnect()
{
    _w5500.disconnect();
    return 0;
}

const char *W5500Interface::get_ip_address()
{
    uint32_t ip = _w5500.reg_rd<uint32_t>(SIPR);
    snprintf(ip_string, sizeof(ip_string), "%d.%d.%d.%d", (int)((ip>>24)&0xff), (int)((ip>>16)&0xff), (int)((ip>>8)&0xff), (int)(ip&0xff));
    return ip_string;
}

const char *W5500Interface::get_netmask()
{
    uint32_t netmask = _w5500.reg_rd<uint32_t>(SUBR);
    snprintf(netmask_string, sizeof(netmask_string), "%d.%d.%d.%d", (int)((netmask>>24)&0xff), (int)((netmask>>16)&0xff), (int)((netmask>>8)&0xff), (int)(netmask&0xff));
    return netmask_string;
}

const char *W5500Interface::get_gateway()
{
    uint32_t gateway = _w5500.reg_rd<uint32_t>(GAR);
    snprintf(gateway_string, sizeof(gateway_string), "%d.%d.%d.%d", (int)((gateway>>24)&0xff), (int)((gateway>>16)&0xff), (int)((gateway>>8)&0xff), (int)(gateway&0xff));
    return gateway_string;
}

const char *W5500Interface::get_mac_address()
{
    uint8_t mac[6];
    _w5500.reg_rd_mac(SHAR, mac);
    snprintf(mac_string, sizeof(mac_string), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return mac_string;
}

void W5500Interface::get_mac(uint8_t mac[6])
{
    _w5500.reg_rd_mac(SHAR, mac);
}

nsapi_error_t W5500Interface::socket_open(nsapi_socket_t *handle, nsapi_protocol_t proto)
{
    //a socket is created the same way regardless of the protocol
    int sock_fd = _w5500.new_socket();
    if (sock_fd < 0) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    w5500_socket *h = get_sock(sock_fd);

    if (!h) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    h->proto         = proto;
    h->connected     = false;
    h->callback      = NULL;
    h->callback_data = NULL;

    //new up an int to store the socket fd
    *handle = h;
    DBG("fd: %d\n", sock_fd);
    return 0;
}

//void W5500Interface::signal_event(nsapi_socket_t handle)
//{
////    DBG("fd: %d\n", SKT(handle)->fd);
////    if (SKT(handle)->callback != NULL) {
////        SKT(handle)->callback(SKT(handle)->callback_data);
////    }
//	if (handle == NULL) return;
//    w5500_socket *socket = (w5500_socket *)handle;
//    w5500_sockets[socket->fd].callback(w5500_sockets[socket->fd].callback_data);
//}

nsapi_error_t W5500Interface::socket_close(nsapi_socket_t handle)
{
    if (handle == NULL) return 0;
    DBG("fd: %d\n", SKT(handle)->fd);
    _w5500.close(SKT(handle)->fd);

    SKT(handle)->fd = -1;

    return 0;
}

nsapi_error_t W5500Interface::socket_bind(nsapi_socket_t handle, const SocketAddress &address)
{
    if (handle < 0) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    DBG("fd: %d, port: %d\n", SKT(handle)->fd, address.get_port());

    switch (SKT(handle)->proto) {
        case NSAPI_UDP:
            // set local port
            if (address.get_port() != 0) {
                _w5500.setLocalPort( SKT(handle)->fd, address.get_port() );
            } else {
                udp_local_port++;
                _w5500.setLocalPort( SKT(handle)->fd, udp_local_port );
            }
            // set udp protocol
            _w5500.setProtocol(SKT(handle)->fd, UDP);
            _w5500.scmd(SKT(handle)->fd, OPEN);
            /*
                        uint8_t tmpSn_SR;
                		tmpSn_SR = _w5500.sreg<uint8_t>(SKT(handle)->fd, Sn_SR);
            		    DBG("open socket status: %2x\n", tmpSn_SR);
            */
            return 0;
        case NSAPI_TCP:
            listen_port = address.get_port();
            // set TCP protocol
            _w5500.setProtocol(SKT(handle)->fd, TCP);
            // set local port
            _w5500.setLocalPort( SKT(handle)->fd, address.get_port() );
            // connect the network
            _w5500.scmd(SKT(handle)->fd, OPEN);
            return 0;
    }

    return NSAPI_ERROR_DEVICE_ERROR;
}

nsapi_error_t W5500Interface::socket_listen(nsapi_socket_t handle, int backlog)
{
    DBG("fd: %d\n", SKT(handle)->fd);
    if (SKT(handle)->fd < 0) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    /*    if (backlog != 1) {
            return NSAPI_ERROR_NO_SOCKET;
        }
    */
    _mutex.lock();
    _w5500.scmd(SKT(handle)->fd, LISTEN);
    _mutex.unlock();
    return 0;
}

nsapi_size_or_error_t W5500Interface::socket_connect(nsapi_socket_t handle, const SocketAddress &address)
{
    DBG("fd: %d\n", SKT(handle)->fd);
    //check for a valid socket
    _mutex.lock();

    if (SKT(handle)->fd < 0) {
        _mutex.unlock();
        return NSAPI_ERROR_NO_SOCKET;
    }

    //before we attempt to connect, we are not connected
    SKT(handle)->connected = false;

    //try to connect
    if (!_w5500.connect(SKT(handle)->fd, address.get_ip_address(), address.get_port(), w5500_WAIT_TIMEOUT)) {
        _mutex.unlock();
        return -1;
    }

    //we are now connected
    SKT(handle)->connected = true;
    _mutex.unlock();

    return 0;
}

nsapi_error_t W5500Interface::socket_accept(nsapi_socket_t server, nsapi_socket_t *handle, SocketAddress *address)
{
    SocketAddress _addr;

    DBG("fd: %d\n", SKT(handle)->fd);
    if (SKT(server)->fd < 0) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    SKT(server)->connected = false;

    Timer t;
    t.reset();
    t.start();

    while(1) {
        if (t.read_ms() > w5500_ACCEPT_TIMEOUT) {
            DBG("W5500Interface::socket_accept, timed out\r\n");
            return NSAPI_ERROR_WOULD_BLOCK;
        }
        if (_w5500.is_connected(SKT(server)->fd)) break;
    }

    //get socket for the connection
    *handle = get_sock(SKT(server)->fd);

    if (!(*handle)) {
        error("No more sockets for binding");
        return NSAPI_ERROR_NO_SOCKET;
    }

    //give it all of the socket info from the server
    SKT(*handle)->proto     = SKT(server)->proto;
    SKT(*handle)->connected = true;

    if (address) {
        uint32_t ip = _w5500.sreg<uint32_t>(SKT(*handle)->fd, Sn_DIPR);
        char host[17];
        snprintf(host, sizeof(host), "%d.%d.%d.%d", (int)((ip>>24)&0xff), (int)((ip>>16)&0xff), (int)((ip>>8)&0xff), (int)(ip&0xff));
        int port = _w5500.sreg<uint16_t>(SKT(*handle)->fd, Sn_DPORT);

        _addr.set_ip_address(host);
        _addr.set_port(port);
        *address = _addr;
    }


    //create a new tcp socket for the server
    SKT(server)->fd = _w5500.new_socket();
    if (SKT(server)->fd < 0) {
        error("No more sockets for listening");
        //return NSAPI_ERROR_NO_SOCKET;
        // already accepted socket, so return 0, but there is no listen socket anymore.
        return 0;
    }

    SKT(server)->proto     = NSAPI_TCP;
    SKT(server)->connected = false;

    _addr.set_port(listen_port);

    // and then, for the next connection, server socket should be assigned new one.
    if (socket_bind(server, _addr) < 0) {
        error("No more sockets for listening");
        //return NSAPI_ERROR_NO_SOCKET;
        // already accepted socket, so return 0, but there is no listen socket anymore.
        return 0;
    }

    if (socket_listen(server, 1) < 0) {
        error("No more sockets for listening");
        // already accepted socket, so return 0, but there is no listen socket anymore.
        return 0;
    }

    return 0;
}

nsapi_size_or_error_t W5500Interface::socket_send(nsapi_socket_t handle, const void *data, nsapi_size_t size)
{
    DBG("fd: %d\n", SKT(handle)->fd);
    //INFO("fd: %d\n", SKT(handle)->fd);

    nsapi_size_t writtenLen = 0;
    int ret;
    _mutex.lock();
    while (writtenLen < size) {
        int _size =  _w5500.wait_writeable(SKT(handle)->fd, w5500_WAIT_TIMEOUT);
        if (_size < 0) {
            _mutex.unlock();
            return NSAPI_ERROR_WOULD_BLOCK;
        }
        if (_size > (size-writtenLen)) {
            _size = (size-writtenLen);
        }
        ret = _w5500.send(SKT(handle)->fd, (char*)(data+writtenLen), (int)_size);
        if (ret < 0) {
            DBG("returning error -1\n");
            _mutex.unlock();
            return -1;
        }
        writtenLen += ret;
    }
    _mutex.unlock();
    return writtenLen;
}

nsapi_size_or_error_t W5500Interface::socket_recv(nsapi_socket_t handle, void *data, nsapi_size_t size)
{
    int recved_size = 0;
    //int idx;
    nsapi_size_t _size;
    nsapi_size_or_error_t err;

    DBG("fd: %d\n", SKT(handle)->fd);
    //INFO("fd: %d\n", SKT(handle)->fd);
    // add to cover exception.
    _mutex.lock();
    if ((SKT(handle)->fd < 0) || !SKT(handle)->connected) {
        _mutex.unlock();
        return -1;
    }
    DBG("fd: connected is %d\n", SKT(handle)->connected);

     while(1) {
        _size = _w5500.wait_readable(SKT(handle)->fd, w5500_WAIT_TIMEOUT);
        DBG("fd: _size %d\n", _size);

        if (_size < 0) {
            if(recved_size > 0){
                err = recved_size;
                //INFO("recved_size : %d\n",recved_size);
                break;
            }
            _mutex.unlock();
            return NSAPI_ERROR_WOULD_BLOCK;
        }

        if (_size > (size - recved_size)) {
            _size = (size - recved_size);
        }

        if (_size == 0 && recved_size !=0 ){
            _mutex.unlock();
            return recved_size;
        }


        err = _w5500.recv(SKT(handle)->fd, (char*)(data + recved_size), (int)_size);
//	    printf("[TEST 400] : %d\r\n",recved_size);
//	    for(idx=0; idx<16; idx++)
//	    {
//	        printf(" %02x",((uint8_t*)data)[idx+recved_size]);
//	    }
//	    printf("\r\n");

        DBG("rv: %d\n", err);
        //INFO("rv: %d\n",err);
        recved_size += _size;
    }

#if w5500_INTF_DBG
    if (err > 0) {
        debug("[socket_recv] buffer:");
        for(int i = 0; i < err; i++) {
            if ((i%16) == 0) {
                debug("\n");
            }
            debug(" %02x", ((uint8_t*)data)[i]);
        }
        if ((err-1%16) != 0) {
            debug("\n");
        }
    }
#endif
    _mutex.unlock();
    return err;
}

nsapi_size_or_error_t W5500Interface::socket_sendto(nsapi_socket_t handle, const SocketAddress &address,
        const void *data, nsapi_size_t size)
{
    _mutex.lock();
    DBG("fd: %d, ip: %s:%d\n", SKT(handle)->fd, address.get_ip_address(), address.get_port());
    if (_w5500.is_closed(SKT(handle)->fd)) {
        nsapi_error_t err = socket_bind(handle, address);
        if (err < 0 ) {
            DBG("failed to bind socket: %d\n", err);
            _mutex.unlock();
            return err;
        }
    }
    //compare with original: int size = eth->wait_writeable(_sock_fd, _blocking ? -1 : _timeout, length-1);
    int len = _w5500.wait_writeable(SKT(handle)->fd, w5500_WAIT_TIMEOUT, size-1);
    if (len < 0) {
        DBG("error: NSAPI_ERROR_WOULD_BLOCK\n");
        _mutex.unlock();
        return NSAPI_ERROR_WOULD_BLOCK;;
    }

    // set remote host
    _w5500.sreg_ip(SKT(handle)->fd, Sn_DIPR, address.get_ip_address());
    // set remote port
    _w5500.sreg<uint16_t>(SKT(handle)->fd, Sn_DPORT, address.get_port());

    nsapi_size_or_error_t err = _w5500.send(SKT(handle)->fd, (const char*)data, size);
    DBG("rv: %d, size: %d\n", err, size);

#if w5500_INTF_DBG
    if (err > 0) {
        debug("[socket_sendto] data: ");
        for(int i = 0; i < err; i++) {
            if ((i%16) == 0) {
                debug("\n");
            }
            debug(" %02x", ((uint8_t*)data)[i]);
        }
        if ((err-1%16) != 0) {
            debug("\n");
        }
    }
#endif
    _mutex.unlock();
    return err;
}

nsapi_size_or_error_t W5500Interface::socket_recvfrom(nsapi_socket_t handle, SocketAddress *address,
        void *buffer, nsapi_size_t size)
{
    DBG("fd: %d\n", SKT(handle)->fd);
    //check for null pointers
    if (buffer == NULL) {
        DBG("buffer is NULL; receive is ABORTED\n");
        return -1;
    }

    _mutex.lock();
    uint8_t info[8];
    int len = _w5500.wait_readable(SKT(handle)->fd, w5500_WAIT_TIMEOUT, sizeof(info));
    if (len < 0) {
        DBG("error: NSAPI_ERROR_WOULD_BLOCK\n");
        _mutex.unlock();
        return NSAPI_ERROR_WOULD_BLOCK;
    }

    //receive endpoint information
    _w5500.recv(SKT(handle)->fd, (char*)info, sizeof(info));

    char addr[17];
    snprintf(addr, sizeof(addr), "%d.%d.%d.%d", info[0], info[1], info[2], info[3]);
    uint16_t port = info[4]<<8|info[5];
    // original behavior was to terminate execution if address is NULL
    if (address != NULL) {
        //DBG("[socket_recvfrom] warn: addressis NULL");
        address->set_ip_address(addr);
        address->set_port(port);
    }

    nsapi_size_t udp_size = info[6]<<8|info[7];

    if (udp_size > (len-sizeof(info))) {
        DBG("error: udp_size > (len-sizeof(info))\n");
        _mutex.unlock();
        return -1;
    }

    //receive from socket
    nsapi_size_or_error_t err = _w5500.recv(SKT(handle)->fd, (char*)buffer, udp_size);
    DBG("rv: %d\n", err);

#if w5500_INTF_DBG
    if (err > 0) {
        debug("[socket_recvfrom] buffer:");
        for(int i = 0; i < err; i++) {
            if ((i%16) == 0) {
                debug("\n");
            }
            debug(" %02x", ((uint8_t*)buffer)[i]);
        }
        if ((err-1%16) != 0) {
            debug("\n");
        }
    }
#endif

    _mutex.unlock();
    return  err;
}

void W5500Interface::socket_attach(void *handle, void (*callback)(void *), void *data)
{
//	if (handle == NULL) return;
//	DBG("fd: %d, callback: %p\n", SKT(handle)->fd, callback);
//	SKT(handle)->callback       = callback;
//	SKT(handle)->callback_data  = data;

    if (handle == NULL) return;
    _mutex.lock();
    w5500_socket *socket = (w5500_socket *)handle;
    w5500_sockets[socket->fd].callback = callback;


    w5500_sockets[socket->fd].callback_data = data;
    _mutex.unlock();
}

void W5500Interface::event()
{
    for(int i=0; i<MAX_SOCK_NUM; i++){
        if (w5500_sockets[i].callback) {
            w5500_sockets[i].callback(w5500_sockets[i].callback_data);
        }
    }
}

nsapi_error_t W5500Interface::gethostbyname(const char *host,
        SocketAddress *address, nsapi_version_t version)
{
    DBG("DNS process %s", host);
    bool isOK = dns.lookup(host);
    if (isOK) {
        DBG("is ok\n");
        DBG(" IP: %s\n", dns.get_ip_address());
    } else {
        DBG(" IP is not ok\n");
        return NSAPI_ERROR_DNS_FAILURE;
    }

    if ( !address->set_ip_address(dns.get_ip_address()) ) {
        return NSAPI_ERROR_DNS_FAILURE;
    }
    return NSAPI_ERROR_OK;
}

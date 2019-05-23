#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/route/link.h>


#include <linux/if_arp.h>
#include <linux/nl80211.h>

#include "ieee80211.h"
#include "cvwifiguest.h"

#include <iostream>
#include <thread>


/* allow calling non static function from static function */
CallFromStaticFunc * VWifiGuest::forward = nullptr ;



void VWifiGuest::set_all_rates_invalid(struct hwsim_tx_rate *tx_rate)
{
	int i;

	/* Set up all unused rates to be -1 */
	for (i = 0; i < IEEE80211_MAX_RATES_PER_TX; i++) {
		tx_rate[i].idx = -1;
		tx_rate[i].count = 0;
	}
}



int VWifiGuest::send_tx_info_frame_nl(struct ether_addr *src, unsigned int flags, int signal, struct hwsim_tx_rate *tx_attempts, unsigned long cookie)
{
	struct nl_msg *msg = nullptr;
	int rc;

	msg = nlmsg_alloc();

	if (!msg) {
		
		std::cout << "Error allocating new message MSG !" << std::endl ;
		nlmsg_free(msg);
		return 0;
	}
	
	if (m_family_id < 0){
		
#ifdef DEBUG
		std::cout << __func__ <<  "m_family_id < 0" << std::endl ;
#endif
		nlmsg_free(msg);
		return 0;
	}

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, m_family_id,
			0, NLM_F_REQUEST, HWSIM_CMD_TX_INFO_FRAME, VERSION_NR);

	/* i have to ack the src the driver expects
	 * so there are no mac address modifications here
	 */
	rc = nla_put(msg, HWSIM_ATTR_ADDR_TRANSMITTER, sizeof(struct ether_addr), src);
	rc = nla_put_u32(msg, HWSIM_ATTR_FLAGS, flags);
	rc = nla_put_u32(msg, HWSIM_ATTR_SIGNAL, signal);
	rc = nla_put(msg, HWSIM_ATTR_TX_INFO, IEEE80211_MAX_RATES_PER_TX * sizeof(struct hwsim_tx_rate), tx_attempts);
	rc = nla_put_u64(msg, HWSIM_ATTR_COOKIE, cookie);

	if (rc != 0) {
		printf("Error filling payload\n");
		nlmsg_free(msg);
		return 0;
	}

	nl_send_auto_complete(m_sock, msg);
	
	nlmsg_free(msg);

	return 1;
}


int VWifiGuest::process_messages_cb(struct nl_msg *msg, void *arg){

	forward->process_messages(msg,arg);	

}


int VWifiGuest::process_messages(struct nl_msg *msg, void *arg)
{

#ifdef DEBUG
	std::cout << __func__ << std::endl ;
#endif

	int msg_len;
	struct nlattr *attrs[HWSIM_ATTR_MAX + 1];
	struct nlmsghdr * nlh;
	struct genlmsghdr * gnlh;
	struct nlmsgerr *err;
	struct ether_addr *src;
	struct ether_addr *dst;
	unsigned int flags;
	struct hwsim_tx_rate *tx_rates;
	unsigned long cookie;
	struct hwsim_tx_rate tx_attempts[IEEE80211_MAX_RATES_PER_TX];
	int round;
	int tx_ok;
	int counter;
	int signal;
	char *data;
	struct ether_addr framesrc;
	struct ether_addr framedst;

	char addr[18];
	int bytes;

	nlh = nlmsg_hdr(msg);
	gnlh = (struct genlmsghdr *) nlmsg_data(nlh);
	memset(addr, 0, 18);

	/* get message length needed for sendto */
	msg_len = nlh->nlmsg_len;

	if (nlh->nlmsg_type != m_family_id) 
		return 1;
	
	if (nlh->nlmsg_type == NLMSG_ERROR) {
		err = (struct nlmsgerr *) nlmsg_data(nlh);
		return err->error ;
	}


	/* ignore if anything other than a frame
	do we need to free the msg? */
	if (!(gnlh->cmd == HWSIM_CMD_FRAME))
		return 1;

	/* processing original HWSIM_CMD_FRAME */
	genlmsg_parse(nlh, 0, attrs, HWSIM_ATTR_MAX, NULL);

	/* this check was duplicated below in a second if statement, now gone */
	if (!(attrs[HWSIM_ATTR_ADDR_TRANSMITTER]))
		return 1;


	if (!(attrs[HWSIM_ATTR_ADDR_RECEIVER]))
		std::cout << "hwsim dst mac is not present" << std::endl ;

	/* we get the attributes*/
	dst = (struct ether_addr *)nla_data(attrs[HWSIM_ATTR_ADDR_RECEIVER]);
	src = (struct ether_addr *)nla_data(attrs[HWSIM_ATTR_ADDR_TRANSMITTER]);
	flags = nla_get_u32(attrs[HWSIM_ATTR_FLAGS]);
	tx_rates = (struct hwsim_tx_rate *)nla_data(attrs[HWSIM_ATTR_TX_INFO]);
	cookie = nla_get_u64(attrs[HWSIM_ATTR_COOKIE]);

//	mac_address_to_string(addr, src);
//	std::cout << "src: " << addr << std::endl ;
	
//	if (!dst){
	
//		mac_address_to_string(addr, dst);
//		std::cout << "dst: " << addr << std::endl;
//	}

	round = 0;
	tx_ok = 0;

	/* We prepare the tx_attempts struct */
	set_all_rates_invalid(tx_attempts);

	while (round < IEEE80211_MAX_RATES_PER_TX &&  tx_rates[round].idx != -1 && tx_ok != 1) {

		counter = 1;

		/* tx_rates comes from the driver...
		 * that means that the receiving ends gets this info
		 * and can use it
		 */

		/* Set rate index and flags used for this round */
		tx_attempts[round].idx = tx_rates[round].idx;

		while (counter <= tx_rates[round].count && tx_ok != 1) {
			tx_attempts[round].count = counter;
			counter++;
		}
		round++;
	}

	/* round -1 is the last element of the array */
	/* this is the signal sent to the sender, not the receiver */
	signal = -10;
	/* Let's flag this frame as ACK'ed */
	/* whatever that means... */
	flags |= HWSIM_TX_STAT_ACK;
	/* this has to be an ack the driver expects */
	/* what does the driver do with these values? can i remove them? */
	send_tx_info_frame_nl(src, flags, signal, tx_attempts,cookie);

	/*
	 * no need to send a tx info frame indicating failure with a
	 * signal of 0 - that was done in the tx code i took this from
	 * if i check for ack messages than i could add a failure message
	 */

	/* we are now done with our code addition which sends the ack */

	/* we get the attributes*/
	data = (char *)nla_data(attrs[HWSIM_ATTR_FRAME]);


	/* copy source address from frame */
	/* if we rebuild the nl msg, this can change */
	memcpy(&framesrc, data + 10, ETH_ALEN);

	/* copy dst address from frame */
	memcpy(&framedst, data + 4, ETH_ALEN);

	
	//mac_address_to_string(addr, &framesrc);
	//std::cout << "frame src: " << addr << std::endl;

	//mac_address_to_string(addr, &framedst);
	//std::cout << "frame dst: " << addr << std::endl;


	/* compare tx src to frame src, update TX src ATTR in msg if needed */
	/* if we rebuild the nl msg, this can change */
	if (memcmp(&framesrc, src, ETH_ALEN) != 0) {

#ifdef DEBUG	
		std::cout << "updating the TX src ATTR" << std::endl ; 
#endif
		/* copy dest address from frame to nlh */
		memcpy((char *)nlh + 24, &framesrc, ETH_ALEN);
	}


	mac_address_to_string(addr, src);
	std::cout << "src: " << addr << std::endl ;
	mac_address_to_string(addr, &framesrc);
	std::cout << "frame src: " << addr << std::endl;



	/* here code of david to send 	(char *)nlh with  msg_len as size*/ 



	return 0 ;
}


int VWifiGuest::send_register_msg(void)
{
	struct nl_msg *msg;

	if (! check_if_netlink_initialized())
		return 0 ;


	msg = nlmsg_alloc();

	if (!msg) {
		std::cout << "Error allocating new message MSG!" << std::endl ;
		return 0;
	}

	// It may be not useful since I already check it with check_if_netlink_initialized() above
	/*if (m_family_id < 0) {
		nlmsg_free(msg);
		return 0;
	}*/

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, m_family_id,0, NLM_F_REQUEST, HWSIM_CMD_REGISTER, VERSION_NR);

	if (nl_send_auto(m_sock, msg) < 0)
	{
		nlmsg_free(msg);
		return 0 ;
	}

	nl_complete_msg(m_sock,msg);
	
	if (nl_send(m_sock, msg) < 0)
	{
		nlmsg_free(msg);
		return 0 ;
	}

	//nl_send_auto_complete(m_sock, msg);
	nlmsg_free(msg);

	return 1;
}


// free better m_cb and m_sock
// improve the stoping process
int VWifiGuest::init_netlink(void)
{

	int nlsockfd;
	struct timeval tv;


	if (check_if_netlink_initialized())
		return 0 ;

//	m_cb = nl_cb_alloc(NL_CB_DEBUG);
	m_cb = nl_cb_alloc(NL_CB_CUSTOM);
	
	if (!m_cb) {
		std::cout << "Error allocating netlink callbacks" << std::endl ;
		return 0;
	}

	m_sock = nl_socket_alloc_cb(m_cb);
	if (!m_sock) {
		std::cout << "Error allocationg netlink socket" << std::endl;
		return 0;
	}

	/* disable auto-ack from kernel to reduce load */
	nl_socket_disable_auto_ack(m_sock);
	
	if(genl_connect(m_sock) < 0)
		return 0 ;

	m_family_id = genl_ctrl_resolve(m_sock, "MAC80211_HWSIM");


	while (m_family_id < 0 ) {

		if(! check_if_started())
			return 0 ;
#ifdef DEBUG
		std::cout << "Family MAC80211_HWSIM not registered" << std::endl ;
#endif

		using namespace  std::chrono_literals;
		std::this_thread::sleep_for(1s);

		m_family_id = genl_ctrl_resolve(m_sock, "MAC80211_HWSIM");
	}

	// add check_if_started() return 0  here in order to avoid the next steps, thus kill him earlier

	nl_cb_set(m_cb, NL_CB_MSG_IN, NL_CB_CUSTOM, &process_messages_cb, NULL);
	nlsockfd = nl_socket_get_fd(m_sock);

	tv.tv_sec = 1;
	tv.tv_usec = 0;

	if (setsockopt(nlsockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("setsockopt");
	}

	
	setInitialized(1) ;
	return 1;
}



/**
 *      @brief Send a cloned frame to the kernel space driver.
 *	This will send a frame to the driver using netlink.
 *	It is received by hwsim with hwsim_cloned_frame_received_nl()
 *	This is taken from wmediumd and modified. It is called after the
 *	message has been received from wmasterd.
 *	@param dst - mac address of receving radio
 *	@param data - frame data
 *	@param data_len - length of frame
 *	@param rate_idx - number of attempts
 *	@param signal - signal strength
 *	@param freq - frequency
 *	@return success or failure
 */
//int send_cloned_frame_msg(struct ether_addr *dst, char *data, int data_len,
//		int rate_idx, int signal, uint32_t freq)
//{
//	int rc;
//	struct nl_msg *msg;
//	char addr[18];
//	int bytes;
//
//	msg = nlmsg_alloc();
//
//	if (!msg) {
//		printf("Error allocating new message MSG!\n");
//		goto out;
//	}
//	if (family_id < 0)
//		goto out;
//
//	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family_id, 0, NLM_F_REQUEST, HWSIM_CMD_FRAME, VERSION_NR);
//
//	rc = nla_put(msg, HWSIM_ATTR_ADDR_RECEIVER, sizeof(struct ether_addr), dst);
//	rc = nla_put(msg, HWSIM_ATTR_FRAME, data_len, data);
//	rc = nla_put_u32(msg, HWSIM_ATTR_RX_RATE, rate_idx);
//	rc = nla_put_u32(msg, HWSIM_ATTR_SIGNAL, signal);
//	if (freq)
//		rc = nla_put_u32(msg, HWSIM_ATTR_FREQ, freq);
//	/* this signal rate will not match the signal acked to the sender
//	 * unless we set the same rate in both functions. normally,
//	 * the calling function determines this signal, and could
//	 * send the info back to the transmitting radio via wmasterd
//	 */
//
//	if (rc != 0) {
//		printf("Error filling payload\n");
//		goto out;
//	}
//	/*
//	printf("#### welled -> hwsim nlmsg cloned beg ####\n");
//	struct nlmsghdr *nlh = nlmsg_hdr(msg);
//	struct genlmsghdr *gnlh = nlmsg_data(nlh);
//	struct nlattr *attrs[HWSIM_ATTR_MAX + 1];
//	genlmsg_parse(nlh, 0, attrs, HWSIM_ATTR_MAX, NULL);
//	nlh_print(nlh);
//	gnlh_print(gnlh);
//	attrs_print(attrs);
//	printf("#### welled -> hwsim nlmsg cloned end ####\n");
//	*/
//	bytes = nl_send_auto_complete(sock, msg);
//	nlmsg_free(msg);
//	if (verbose) {
//		mac_address_to_string(addr, dst);
//		printf("sent %d bytes to %s\n", bytes, addr);
//	}
//	return 0;
//out:
//	nlmsg_free(msg);
//	return -1;
//}
//



void VWifiGuest::recv_from_server(){

#ifdef DEBUG

	std::cout <<  __func__ << std::endl ;
#endif



	char buf[1024];
	struct timeval tv; /* timer to break out of recvfrom function */
	int bytes;
	
	struct nlmsghdr *nlh;
	struct genlmsghdr *gnlh;
	struct nlattr *attrs[HWSIM_ATTR_MAX + 1];
	uint32_t freq;
	struct ether_addr *src;
	unsigned int data_len;
	char *data;
	int rate_idx;
	int signal;
	struct ether_addr *dst;	/* stores user mac */
	int i;
	char addr[18];
	struct ether_addr radiomac;
	struct device_node *node;
	struct ether_addr framedst;
	int should_ack;
	int retval;
	int distance;

	tv.tv_sec = 1;
	tv.tv_usec = 0;


	/* receive bytes packets from server and store them in buf */


	/* netlink header */
	nlh = (struct nlmsghdr *)buf;

	/* generic netlink header */
	gnlh = (struct genlmsghdr*)nlmsg_data(nlh);


	/* exit if the message does not contain frame data */
	if (gnlh->cmd != HWSIM_CMD_FRAME) {
	
#ifdef DEBUG	
		printf("Error - received no frame data in message\n");
#endif
		return ;
	}

	/* we get the attributes*/
	genlmsg_parse(nlh, 0, attrs, HWSIM_ATTR_MAX, NULL);

	if (attrs[HWSIM_ATTR_FREQ])
		freq = nla_get_u32(attrs[HWSIM_ATTR_FREQ]);
	else
		freq = 0;

	
	/* TODO: check whether this is an ACK FRAME
	 * it would have TX info, COOKIE, and SIGNAL data
	 * i could then sent tx info message to the driver
	 */

	/*  ignore HWSIM_CMD_TX_INFO_FRAME for now */
	if (gnlh->cmd == HWSIM_CMD_TX_INFO_FRAME) {
		printf("Ignoring HWSIM_CMD_TX_INFO_FRAME\n");
		return ;
	}

	if (!attrs[HWSIM_ATTR_ADDR_TRANSMITTER]) {
		printf("Error - message does not contain tx address\n");
		return;
	}
	
	src = (struct ether_addr *)nla_data(attrs[HWSIM_ATTR_ADDR_TRANSMITTER]);

	data_len = nla_len(attrs[HWSIM_ATTR_FRAME]);
	data = (char *)nla_data(attrs[HWSIM_ATTR_FRAME]);


	/*
 
	 * if we want to get crazy, we could send an ack back to the server
	 * and then the server could send an ack back to the orignal client
	 * the server would have to keep track of the mac/client pairings
	*/

	/* we extract and handle a distance here */

	/* copy dst address from frame */
	memcpy(&framedst, data + 4, ETH_ALEN);

#ifdef DEBUG
		mac_address_to_string(addr, &framedst);
		std::cout << "frame dst:" << addr std::endl;
#endif

 			
	//	send_cloned_frame_msg(dst, data, data_len,rate_idx, signal, freq);
				
	//	generate_ack_frame(freq, src, dst);
}



void VWifiGuest::recv_msg_from_hwsim_loop_start(){

#ifdef DEBUG

	std::cout <<  __func__ << std::endl ;
#endif

	if (! check_if_netlink_initialized())
		return  ;
	
	/* loop for waiting  incoming msg from hwsim driver*/
	while (true) {


		if(!check_if_started())
			break ;
	
		nl_recvmsgs_default(m_sock);
	}
}




void VWifiGuest::recv_msg_from_server_loop_start(){

#ifdef DEBUG

	std::cout <<  __func__ << std::endl ;
#endif

	if (! check_if_netlink_initialized())
		return  ;


	while(true){
	
		if(!check_if_started())
			break ;
	
		//eecv_from_server();
	}
}



int VWifiGuest::start(){

#ifdef DEBUG

	std::cout <<  __func__ << std::endl ;
#endif

	/* allows calls from  static callback to non static member function */ 
	forward = new CallFromStaticFunc(this);

	m_started = true ;
	
	/* init netlink will loop until driver is loaded */
	if ( ! init_netlink()){
		
		std::cout << "ERROR: could not initialize netlink" << std::endl;
		return 0 ;
	} 



	/* Send a register msg to the kernel */
	if (!send_register_msg())
		return 0 ;

	

	std::cout << "Registered with family MAC80211_HWSIM" << std::endl;

	
	std::thread hwsimloop(&VWifiGuest::recv_msg_from_hwsim_loop_start,this);
	std::thread serverloop(&VWifiGuest::recv_msg_from_server_loop_start,this);

	
	hwsimloop.join();
	serverloop.join();

//	clean_all() ;

	return 1;

}


int VWifiGuest::stop(){

		m_mutex_ctrl_run.lock();
		m_started = false ;
		m_mutex_ctrl_run.unlock();
	
}


void VWifiGuest::clean_all(){

#ifdef DEBUG

	std::cout <<  __func__ << std::endl ;
#endif


	nl_close(m_sock);
	nl_socket_free(m_sock);
	nl_cb_put(m_cb);
	setInitialized(0);

}

VWifiGuest::VWifiGuest() : m_initialized(0),m_started(0),m_sock(nullptr),m_cb(nullptr) {

}


VWifiGuest::~VWifiGuest(){


#ifdef DEBUG

	std::cout <<  __func__ << std::endl ;
#endif

	clean_all();
	if (forward)
		delete forward ;


}


void VWifiGuest::setInitialized(int val){

	m_mutex_init.lock();
	m_initialized = val ;
	m_mutex_init.unlock();

}


bool VWifiGuest::check_if_netlink_initialized() {


	m_mutex_init.lock();
	
	if (!m_initialized){
		m_mutex_init.unlock() ;
		return false ;
	}

	m_mutex_init.unlock();
	return true ;
}



bool VWifiGuest::check_if_started(){

	m_mutex_ctrl_run.lock();
		
		if(! m_started){
			m_mutex_ctrl_run.unlock();
			return false ;

		}
		
	m_mutex_ctrl_run.unlock();

	return true ;

}

void VWifiGuest::mac_address_to_string(char *address, struct ether_addr *mac)
{
	sprintf(address, "%02X:%02X:%02X:%02X:%02X:%02X",
		mac->ether_addr_octet[0], mac->ether_addr_octet[1], mac->ether_addr_octet[2],
		mac->ether_addr_octet[3], mac->ether_addr_octet[4], mac->ether_addr_octet[5]);
}

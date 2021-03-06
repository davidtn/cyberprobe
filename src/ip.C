
#include <stdint.h>

#include <cybermon/ip.h>
#include <cybermon/tcp.h>
#include <cybermon/udp.h>
#include <cybermon/icmp.h>
#include <cybermon/manager.h>

using namespace cybermon;

const unsigned int ip4_context::max_frag_list_len = 50;

void ip::process_ip4(manager& mgr, context_ptr c, pdu_iter s, pdu_iter e)
{

    if ((e - s) < 20) throw exception("Packet too small for IPv4");

    unsigned int length = (s[2] << 8) + s[3];

    // Packet is allowed to be too long, but not too short.  May have been
    // padded by Ethernet.
    if ((e - s) < length) throw exception("Truncated IP packet");

    // Stuff from the IP header.
    uint8_t ihl = s[0] & 0x0f;
    ip4_id id = (s[4] << 8) + s[5];
    uint8_t flags = s[6] >> 5;
    uint16_t frag_offset = 8 * (((s[6] & 0x1f) << 8) + s[7]);
    uint8_t protocol = s[9];

    if (ihl < 5) throw exception("IP packet IHL is invalid");

    uint8_t header_length = ihl * 4;
    if ((e - s) < header_length) throw exception("IP packet IHL is invalid");

    // This doesn't work on some systems.
#ifdef CHECK_IP_CHECKSUM
    // Calculate checksum.
    uint16_t checked = calculate_cksum(s, s + header_length);
    if (checked != 0)
	throw exception("IP packet has invalid checksum");
#endif

    // Addresses.
    address src, dest;
    src.set(s + 12, s + 16, NETWORK, IP4);
    dest.set(s + 16, s + 20, NETWORK, IP4);

    // Create the flow address.
    flow_address f(src, dest);

    // Get the IP context.
    ip4_context::ptr fc = ip4_context::get_or_create(c, f);

    // Set / update TTL on the context.
    // 120 seconds.
    fc->set_ttl(context::default_ttl);

    fc->lock.lock();

    // Frag processing if the more_frags option is set.
    bool frag_proc = (flags & 1);

    // Frag processing if we've already seen frags for the IP ID we're looking 
    // at.
    frag_proc |= (fc->h_list.find(id) != fc->h_list.end());

    // FIXME: Manage the queue size!  Timeout etc!

    if (frag_proc) {

	// First things first, clear out old frags.
	while (fc->frags.size() > fc->max_frag_list_len) {

	    // Get the ID from the oldest frag.
	    unsigned long aging_id = fc->frags.front().id;

	    // We're about to throw this frag away, may as well clear out the
	    // frag index and hole_list, cause if they're not complete, they
	    // won't complete now.
	    fc->f_list.erase(aging_id);
	    fc->h_list.erase(aging_id);
	    fc->hdrs_list.erase(aging_id);

	    // Delete the unwanted frag.
	    fc->frags.pop_front();

	}

	// Using RFC815 algorithm.
	unsigned long frag_first = frag_offset;
	unsigned long frag_last = frag_offset + length - header_length;

	// First frag seen?  Just create a hole from 0 .. infinity.
	if (fc->h_list.find(id) == fc->h_list.end()) {
	    fragment_hole fh;
	    fh.first = 0;
	    fh.last = 4000000;	// Just a ridiculously large value.
	    fc->h_list[id].push_back(fh);
	}

	// There's definitely a hole list now.

	// Get the hole list.
	hole_list& hl = fc->h_list[id];

	// Loop through holes.
	hole_list::iterator it = hl.begin();
	while (it != hl.end()) {

	    unsigned long hole_first = it->first;
	    unsigned long hole_last = it->last;
	    
	    // If this frag occurs after the hole, ignore it.
	    if (frag_first > hole_last) { it++; continue; }

	    // If this frag occurs before the hole, ignore it.
	    if (frag_last < hole_first) { it++; continue; }

	    // This frag overlaps with the hole in some way.
	    
	    // Delete current hole.
	    it = hl.erase(it);

	    if (frag_first > hole_first) {
		fragment_hole fh;
		fh.first = hole_first;
		fh.last = frag_first - 1;
		hl.push_back(fh);
	    }

	    // If there are more_frags, we need to add another hole.
	    if ((frag_last < hole_last) && (flags & 1)) {
		fragment_hole fh;
		fh.first = frag_last + 1;
		fh.last = hole_last;
		hl.push_back(fh);
	    }

	    continue;

	}

	// If hole last is empty, we have completed.
	if (hl.empty()) {

	    // Now need to reconstruct the frag.

	    std::vector<unsigned char> pdu;

	    unsigned long pdu_size = 0;

	    fragment_list& fl = fc->f_list[id];

	    unsigned long header_size = fc->hdrs_list[id].size();

	    for(std::list<fragment*>::iterator it2 = fl.begin();
		it2 != fl.end();
		it2++) {

		fragment& f = **it2;

		if ((header_size + f.last) > pdu_size) {
		    pdu_size = header_size + f.last;
		    pdu.resize(pdu_size);
		}

		std::copy((*it2)->frag.begin(), 
			  (*it2)->frag.end(),
			  pdu.begin() + header_size + (*it2)->first);

	    }

	    // Now the frag that triggered this re-assembly.

	    // Resize the PDU.
	    if ((header_size + frag_last) > pdu_size) {
		pdu_size = header_size + frag_last;
		pdu.resize(pdu_size);
	    }

	    // Copy this frag into place.
	    std::copy(s + header_length, e,
		      pdu.begin() + header_length + frag_first);

	    // Now put the header in place.
	    std::copy(fc->hdrs_list[id].begin(), fc->hdrs_list[id].end(), 
		      pdu.begin());

	    // Change the 'more frags' flag in this PDU.
	    pdu[6] = pdu[6] & 0xdf;

	    // Set the length.
	    pdu[2] = (pdu.size() & 0xff00) >> 8;
	    pdu[3] = (pdu.size() & 0xff);

	    // Recalculate checksum.
	    pdu[10] = pdu[11] = 0;
	    uint16_t cksum = calculate_cksum(pdu.begin(), 
					     pdu.begin() + header_length);

	    pdu[10] = (cksum & 0xff00) >> 8;
	    pdu[11] = cksum & 0xff;

	    // Tidy up all the frag stuff, so that frag processing doesn't
	    // go re-entrant.
	    fc->h_list.erase(id);
	    fc->f_list.erase(id);
	    fc->hdrs_list.erase(id);

	    // We now have a complete IP packet!  Process it.
	    ip::process_ip4(mgr, c, pdu.begin(), pdu.end());

	    fc->lock.unlock();

	    return;

	} else {

	    // Put this frag on the queue.
	    fragment f;
	    f.first = frag_first;
	    f.last = frag_last;
	    f.id = id;

	    fc->frags.push_back(f);

	    //FIXME: Is any of this thread safe?

	    // Put the frag on the frag queue.
	    if (frag_first == 0) {

		// Keep the IP header of the first frag.
		fc->hdrs_list[id].assign(s, s + header_length);
		fc->frags.back().frag.assign(s + header_length, e);

	    } else {

		// Otherwise just keep the payload.
		fc->frags.back().frag.assign(s + header_length, e);

	    }

	    // Put the frag in the ID->frag index.
	    fc->f_list[id].push_back(&(fc->frags.back()));

	}

	fc->lock.unlock();

	return;

    }

    fc->lock.unlock();

    // Complete payload, just process it.

    if (protocol == 6)

	// TCP
	tcp::process(mgr, fc, s + header_length, s + length);

    else if (protocol == 17)
	
	// UDP
	udp::process(mgr, fc, s + header_length, s + length);

    else if (protocol == 1)
	
	// ICMP
	icmp::process(mgr, fc, s + header_length, s + length);

    else {
	std::ostringstream buf;
	buf << "IP protocol " << (int) protocol << " not handled.";
	throw exception(buf.str());
    }

}

void ip::process_ip6(manager& mgr, context_ptr c, pdu_iter s, pdu_iter e)
{
    throw exception("IPv6 processing not implemented.");
}

uint16_t ip::calculate_cksum(pdu_iter s, pdu_iter e)
{
    
    pdu_iter ptr = s;

    uint32_t sum = 0;

    // Handle 2-bytes at a time.
    while ((e - ptr) > 1) {
	sum += (ptr[0] << 8) + ptr[1];
	if (sum & 0x80000000)
	    sum = (sum & 0xffff) + (sum >> 16);
	ptr += 2;
    }

    // If a remaining byte, handle that.
    if ((e - ptr) != 0)
	sum += ptr[0];

    while (sum >> 16) {
	sum = (sum & 0xffff) + (sum >> 16);
    }

    return ~sum;

}

void ip::process(manager& mgr, context_ptr c, pdu_iter s, pdu_iter e)
{

  // Packet too small for the IP check, then do nothing.
  if ((e - s) < 1)
    throw exception("Empty packet");

  if ((*s & 0xf0) == 0x40)
      process_ip4(mgr, c, s, e);
  else if ((*s & 0xf0) == 0x60)
      process_ip6(mgr, c, s, e);
  else
      throw exception("Expecting IP, but isn't IPv4 or IPv6");

}

/*
 * packet.{cc,hh} -- a packet structure. In the Linux kernel, a synonym for
 * `struct sk_buff'
 * Eddie Kohler, Robert Morris
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * Further elaboration of this license, including a DISCLAIMER OF ANY
 * WARRANTY, EXPRESS OR IMPLIED, is provided in the LICENSE file, which is
 * also accessible at http://www.pdos.lcs.mit.edu/click/license.html
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <click/packet.hh>
#include <click/glue.hh>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>

#ifdef __KERNEL__

Packet::Packet()
{
  StaticAssert(sizeof(Anno) <= 48);
  panic("Packet constructor");
}

Packet::~Packet()
{
  panic("Packet destructor");
}

WritablePacket *
Packet::make(unsigned headroom, const unsigned char *data, unsigned len,
	     unsigned tailroom)
{
  unsigned size = len + headroom + tailroom;
  if (struct sk_buff *skb = alloc_skb(size, GFP_ATOMIC)) {
    skb_reserve(skb, headroom);	// leave some headroom
    __skb_put(skb, len);	// leave space for data
    if (data) memcpy(skb->data, data, len);
    skb->pkt_type = HOST;
    WritablePacket *q = reinterpret_cast<WritablePacket *>(skb);
    q->clear_annotations();
    return q;
  } else {
    click_chatter("oops, kernel could not allocate memory for skbuff");
    return 0;
  }
}

#else /* !__KERNEL__ */

inline
Packet::Packet()
{
  _use_count = 1;
  _data_packet = 0;
  _head = _data = _tail = _end = 0;
  _destructor = 0;
  clear_annotations();
}

Packet::~Packet()
{
  if (_data_packet)
    _data_packet->kill();
  else if (_head && _destructor)
    _destructor(_head, _end - _head);
  else
    delete[] _head;
  _head = _data = 0;
}

inline WritablePacket *
Packet::make(int, int, int)
{
  return static_cast<WritablePacket *>(new Packet(6, 6, 6));
}

WritablePacket *
Packet::make(unsigned char *data, unsigned len, void (*destruct)(unsigned char *, size_t))
{
  WritablePacket *p = new WritablePacket;
  if (p) {
    p->_head = p->_data = data;
    p->_tail = p->_end = data + len;
    p->_destructor = destruct;
  }
  return p;
}

bool
Packet::alloc_data(unsigned headroom, unsigned len, unsigned tailroom)
{
  unsigned n = len + headroom + tailroom;
  _destructor = 0;
  _head = new unsigned char[n];
  if (!_head)
    return false;
  _data = _head + headroom;
  _tail = _data + len;
  _end = _head + n;
  return true;
}

WritablePacket *
Packet::make(unsigned headroom, const unsigned char *data, unsigned len,
	     unsigned tailroom)
{
  WritablePacket *p = new WritablePacket;
  if (!p)
    return 0;
  if (!p->alloc_data(headroom, len, tailroom)) {
    delete p;
    return 0;
  }
  if (data)
    memcpy(p->data(), data, len);
  return p;
}

#endif /* __KERNEL__ */

//
// UNIQUEIFICATION
//

#ifdef __KERNEL__

Packet *
Packet::clone()
{
  struct sk_buff *nskb = skb_clone(skb(), GFP_ATOMIC);
  return reinterpret_cast<Packet *>(nskb);
}

WritablePacket *
Packet::uniqueify_copy()
{
  struct sk_buff *nskb = skb_copy(skb(), GFP_ATOMIC);
  if (nskb) {
    // all annotations, including IP header annotation, are copied,
    // but IP header will point to garbage if old header was 0
    if (!network_header())
      nskb->nh.raw = nskb->h.raw = 0;
  }
  kill();
  return reinterpret_cast<WritablePacket *>(nskb);
}

#else /* user level */

Packet *
Packet::clone()
{
  Packet *p = Packet::make(6, 6, 6); // dummy arguments: no initialization
  if (!p) return 0;
  p->_use_count = 1;
  p->_data_packet = this;
  p->_head = _head;
  p->_data = _data;
  p->_tail = _tail;
  p->_end = _end;
  p->_destructor = 0;
  p->copy_annotations(this);
  p->_nh.raw = _nh.raw;
  p->_h_raw = _h_raw;
  // increment our reference count because of _data_packet reference
  _use_count++;
  return p;
}

WritablePacket *
Packet::uniqueify_copy()
{
  WritablePacket *p = Packet::make(6, 6, 6); // dummy arguments: no initialization
  if (p) {
    p->_use_count = 1;
    p->_data_packet = 0;
    if (p->alloc_data(headroom(), length(), tailroom())) {
      memcpy(p->_data, _data, _tail - _data);
      p->copy_annotations(this);
      if (_nh.raw) {
	p->_nh.raw = p->_data + network_header_offset();
	p->_h_raw = p->_data + transport_header_offset();
      } else {
	p->_nh.raw = 0;
	p->_h_raw = 0;
      }
    } else {
      delete p;
      p = 0;
    }
  }
  kill();
  return p;
}

#endif


//
// EXPENSIVE_PUSH, EXPENSIVE_PUT
//

/*
 * Prepend some empty space before a packet.
 * May kill this packet and return a new one.
 */
WritablePacket *
Packet::expensive_push(unsigned int nbytes)
{
  static int chatter = 0;
  if (chatter < 5) {
    click_chatter("expensive Packet::push; have %d wanted %d",
                  headroom(), nbytes);
    chatter++;
  }
#ifdef __KERNEL__
  struct sk_buff *nskb = skb_realloc_headroom(skb(), nbytes);
  // new packet guaranteed not to be shared
  WritablePacket *q = reinterpret_cast<WritablePacket *>(nskb);
  if (q) {
    __skb_push(q->skb(), nbytes);
    // skb_realloc_headroom doesn't deal well with null network header
    if (!network_header())
      q->set_network_header(0, 0);
  }
#else
  WritablePacket *q = Packet::make(nbytes + 128, total_data(), total_length(), 0);
  if (q) {
    q->_data += headroom() - nbytes;
    q->_tail -= tailroom();
    q->copy_annotations(this);
    if (network_header())
      q->set_network_header(q->data() + network_header_offset(), network_header_length());
  }
#endif
  kill();
  return q;
}

WritablePacket *
Packet::expensive_put(unsigned int nbytes)
{
  static int chatter = 0;
  if (chatter < 5) {
    click_chatter("expensive Packet::put; have %d wanted %d",
                  tailroom(), nbytes);
    chatter++;
  }
  WritablePacket *q = Packet::make(0, total_data(), total_length(), nbytes + 128);
  if (q) {
#ifdef __KERNEL__
    sk_buff *skb = q->skb();
    skb->tail += nbytes - tailroom();
    skb->data += headroom();
    skb->len += nbytes - tailroom() - headroom();
#else
    q->_tail += nbytes - tailroom();
    q->_data += headroom();
#endif
    q->copy_annotations(this);
    if (network_header())
      q->set_network_header(q->data() + network_header_offset(), network_header_length());
  }
  kill();
  return q;
}

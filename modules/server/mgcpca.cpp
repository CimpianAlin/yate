/**
 * mgcpca.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Media Gateway Control Protocol - Call Agent - also remote data helper
 *  for other protocols
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <yatephone.h>
#include <yatemgcp.h>
#include <yatesig.h>

#include <stdlib.h>

using namespace TelEngine;
namespace { // anonymous

class MGCPCircuit;

class YMGCPEngine : public MGCPEngine
{
public:
    inline YMGCPEngine(const NamedList* params)
	: MGCPEngine(false,0,params)
	{ }
    virtual ~YMGCPEngine();
    virtual bool processEvent(MGCPTransaction* trans, MGCPMessage* msg, void* data);
};

class MGCPWrapper : public DataEndpoint
{
    YCLASS(MGCPWrapper,DataEndpoint)
public:
    MGCPWrapper(CallEndpoint* conn, const char* media, Message& msg);
    ~MGCPWrapper();
    bool sendDTMF(const String& tones);
    void gotDTMF(char tone);
    inline const String& id() const
	{ return m_id; }
    inline const String& ntfyId() const
	{ return m_notify; }
    inline const String& callId() const
	{ return m_master; }
    inline const String& connEp() const
	{ return m_connEp; }
    inline const String& connId() const
	{ return m_connId; }
    inline bool isAudio() const
	{ return m_audio; }
    static MGCPWrapper* find(const CallEndpoint* conn, const String& media);
    static MGCPWrapper* find(const String& id);
    static MGCPWrapper* findNotify(const String& id);
    bool processEvent(MGCPTransaction* tr, MGCPMessage* mm);
    bool processNotify(MGCPTransaction* tr, MGCPMessage* mm, const String& event);
    bool rtpMessage(Message& msg);
    RefPointer<MGCPMessage> sendSync(MGCPMessage* mm, const SocketAddr& address);
    void clearConn();
protected:
    virtual bool nativeConnect(DataEndpoint* peer);
private:
    void addParams(MGCPMessage* mm);
    MGCPTransaction* m_tr;
    RefPointer<MGCPMessage> m_msg;
    String m_connId;
    String m_connEp;
    String m_id;
    String m_notify;
    String m_master;
    bool m_audio;
};

class MGCPSpan : public SignallingCircuitSpan
{
public:
    MGCPSpan(const NamedList& params, const char* name, const MGCPEpInfo& ep);
    virtual ~MGCPSpan();
    inline const String& ntfyId() const
	{ return m_notify; }
    inline bool operational() const
	{ return m_operational; }
    static void* create(const String& type, const NamedList& name);
    static MGCPSpan* findNotify(const String& id);
    bool matchEndpoint(const MGCPEndpointId& ep);
    bool processEvent(MGCPTransaction* tr, MGCPMessage* mm);
    bool processNotify(MGCPTransaction* tr, MGCPMessage* mm, const String& event, const String& requestId);
    bool processRestart(MGCPTransaction* tr, MGCPMessage* mm, const String& method);
private:
    bool init(const NamedList& params);
    void clearCircuits();
    MGCPCircuit* findCircuit(const String& epId, const String& rqId) const;
    void operational(bool active);
    MGCPCircuit** m_circuits;
    unsigned int m_count;
    MGCPEndpointId m_epId;
    bool m_operational;
    String m_notify;
};

class MGCPCircuit : public SignallingCircuit
{
public:
    MGCPCircuit(unsigned int code, MGCPSpan* span);
    virtual ~MGCPCircuit();
    virtual bool status(Status newStat, bool sync);
    inline const String& ntfyId() const
	{ return m_notify; }
    bool processNotify(const String& event);
private:
    Status m_statusReq;
    String m_notify;
};

class RtpHandler : public MessageHandler
{
public:
    RtpHandler(unsigned int prio) : MessageHandler("chan.rtp",prio) { }
    virtual bool received(Message &msg);
};

class SdpHandler : public MessageHandler
{
public:
    SdpHandler(unsigned int prio) : MessageHandler("chan.sdp",prio) { }
    virtual bool received(Message &msg);
};

class DTMFHandler : public MessageHandler
{
public:
    DTMFHandler() : MessageHandler("chan.dtmf",150) { }
    virtual bool received(Message &msg);
};

class MGCPPlugin : public Module
{
public:
    MGCPPlugin();
    virtual ~MGCPPlugin();
    virtual void initialize();
    virtual void statusParams(String& str);
    virtual void statusDetail(String& str);
};

YSIGFACTORY2(MGCPSpan);

static YMGCPEngine* s_engine = 0;
static MGCPEndpoint* s_endpoint = 0;
static String s_defaultEp;

static MGCPPlugin splugin;
static ObjList s_wrappers;
static ObjList s_spans;
static Mutex s_mutex;


// Copy one parameter (if present) with new name
bool copyRename(NamedList& dest, const char* dname, const NamedList& src, const String& sname)
{
    if (!sname)
	return false;
    const NamedString* value = src.getParam(sname);
    if (!value)
	return false;
    dest.addParam(dname,*value);
    return true;
}

YMGCPEngine::~YMGCPEngine()
{
    s_engine = 0;
    s_endpoint = 0;
}

// Process all events of this engine, forward them to wrappers if found
bool YMGCPEngine::processEvent(MGCPTransaction* trans, MGCPMessage* msg, void* data)
{
    MGCPWrapper* wrap = YOBJECT(MGCPWrapper,static_cast<GenObject*>(data));
    MGCPSpan* span = YOBJECT(MGCPSpan,static_cast<GenObject*>(data));
    Debug(this,DebugAll,"YMGCPEngine::processEvent(%p,%p,%p) wrap=%p span=%p [%p]",
	trans,msg,data,wrap,span,this);
    if (!trans)
	return false;
    if (wrap)
	return wrap->processEvent(trans,msg);
    if (span)
	return span->processEvent(trans,msg);
    if (!msg)
	return false;
    if (!data && !trans->outgoing() && msg->isCommand()) {
	while (msg->name() == "NTFY") {
	    const String* rqId = msg->params.getParam("x");
	    const String* event = msg->params.getParam("o");
	    if (null(rqId))
		trans->setResponse(538,"Missing request-id");
	    else if (null(event))
		trans->setResponse(538,"Missing observed events");
	    else if (*rqId == "0") {
		// persistent notification
		Debug(this,DebugInfo,"NTFY '%s' from '%s'",
		    c_str(event),
		    msg->endpointId().c_str());
		MGCPEndpointId id(msg->endpointId());
		bool ok = false;
		if (id.valid()) {
		    s_mutex.lock();
		    ListIterator iter(s_spans);
		    while ((span = static_cast<MGCPSpan*>(iter.get()))) {
			if (span->matchEndpoint(id)) {
			    s_mutex.unlock();
			    ok = span->processNotify(trans,msg,*event,*rqId) || ok;
			    s_mutex.lock();
			}
		    }
		    s_mutex.unlock();
		}
		if (ok) {
		    trans->setResponse(200,"OK");
		    return true;
		}
		break;
	    }
	    else {
		bool ok = false;
		wrap = MGCPWrapper::findNotify(*rqId);
		if (wrap)
		    ok = wrap->processNotify(trans,msg,*event);
		else {
		    span = MGCPSpan::findNotify(*rqId);
		    if (span)
			ok = span->processNotify(trans,msg,*event,*rqId);
		    else {
			trans->setResponse(538,"Unknown request-id");
			return true;
		    }
		}
		if (ok)
		    trans->setResponse(200,"OK");
		else
		    trans->setResponse(539,"Unsupported parameter");
	    }
	    return true;
	}
	if (msg->name() == "RSIP") {
	    const String* method = msg->params.getParam("rm");
	    Debug(this,DebugInfo,"RSIP '%s' from '%s'",
		c_str(method),msg->endpointId().c_str());
	    MGCPEndpointId id(msg->endpointId());
	    bool ok = false;
	    if (id.valid()) {
		if (!method)
		    method = &String::empty();
		s_mutex.lock();
		ListIterator iter(s_spans);
		while ((span = static_cast<MGCPSpan*>(iter.get()))) {
		    if (span->matchEndpoint(id)) {
			s_mutex.unlock();
			ok = span->processRestart(trans,msg,*method) || ok;
			s_mutex.lock();
		    }
		}
		s_mutex.unlock();
	    }
	    if (ok) {
		trans->setResponse(200);
		return true;
	    }
	}
	Debug(this,DebugMild,"Unhandled '%s' from '%s'",
	    msg->name().c_str(),msg->endpointId().c_str());
    }
    return false;
}


MGCPWrapper::MGCPWrapper(CallEndpoint* conn, const char* media, Message& msg)
    : DataEndpoint(conn,media),
      m_tr(0)
{
    Debug(&splugin,DebugAll,"MGCPWrapper::MGCPWrapper(%p,'%s') [%p]",
	conn,media,this);
    m_id = "mgcp/";
    m_id << (unsigned int)::random();
    if (conn)
	m_master = conn->id();
    m_master = msg.getValue("id",(conn ? conn->id().c_str() : (const char*)0));
    m_audio = (name() == "audio");
    m_connEp = msg.getValue("mgcp_endpoint",s_defaultEp);
    s_mutex.lock();
    s_wrappers.append(this);
//    setupRTP(localip,rtcp);
    s_mutex.unlock();
}

MGCPWrapper::~MGCPWrapper()
{
    Debug(&splugin,DebugAll,"MGCPWrapper::~MGCPWrapper() '%s' [%p]",
	name().c_str(),this);
    s_mutex.lock();
    s_wrappers.remove(this,false);
    if (m_tr) {
	m_tr->userData(0);
	m_tr = 0;
    }
    s_mutex.unlock();
    m_msg = 0;
    clearConn();
}

// Process incoming events for this wrapper
bool MGCPWrapper::processEvent(MGCPTransaction* tr, MGCPMessage* mm)
{
    Debug(&splugin,DebugAll,"MGCPWrapper::processEvent(%p,%p) [%p]",
	tr,mm,this);
    if (tr == m_tr) {
	if (!mm || (tr->msgResponse())) {
	    tr->userData(0);
	    m_msg = mm;
	    m_tr = 0;
	}
    }
    else if (mm) {
	if (mm->name() == "NTFY") {
	    String* event = mm->params.getParam("o");
	    if (event && processNotify(tr,mm,*event)) {
		tr->setResponse(200);
		return true;
	    }
	}
    }
    return false;
}

// Process incoming notify events for this wrapper
bool MGCPWrapper::processNotify(MGCPTransaction* tr, MGCPMessage* mm, const String& event)
{
    Debug(&splugin,DebugStub,"MGCPWrapper::processNotify(%p,%p,'%s') [%p]",
	tr,mm,event.c_str(),this);

    if (event.null())
	return false;
    else if (event.find(',') >= 0) {
	// Multiple events
	ObjList* l = event.split(',',false);
	bool ok = false;
	for (ObjList* p = l->skipNull(); p; p = p->skipNext())
	    ok = processNotify(tr,mm,p->get()->toString()) || ok;
	delete l;
	return ok;
    }
    else
	return false;
    return true;
}

// Process local chan.rtp messages for this wrapper
bool MGCPWrapper::rtpMessage(Message& msg)
{
    if (!s_endpoint)
	return false;
    const char* cmd = "MDCX";
    bool extend = false;
    bool fini = msg.getBoolValue("terminate");
    if (fini) {
	if (m_connId.null())
	    return true;
	cmd = "DLCX";
    }
    else if (m_connId.null())
	cmd = "CRCX";
    MGCPEpInfo* ep = s_endpoint->find(m_connEp);
    if (!ep)
	return false;
    RefPointer<MGCPMessage> mm = new MGCPMessage(s_engine,cmd,ep->toString());
    addParams(mm);
    String dir = msg.getValue("direction",m_connId.null() ? "bidir" : "");
    if (dir == "bidir")
	dir = "sendrecv";
    else if (dir == "send")
	dir = "sendonly";
    else if (dir == "receive")
	dir = "recvonly";
    else
	dir.clear();
    if (dir)
	mm->params.addParam("M",dir);
    if (extend && !fini) {
	if (m_connId.null()) {
	    copyRename(mm->params,"x-transport",msg,"transport");
	    copyRename(mm->params,"x-mediatype",msg,"media");
	}
	copyRename(mm->params,"x-localip",msg,"localip");
	copyRename(mm->params,"x-localport",msg,"localport");
	copyRename(mm->params,"x-remoteip",msg,"remoteip");
	copyRename(mm->params,"x-remoteport",msg,"remoteport");
	copyRename(mm->params,"x-payload",msg,"payload");
	copyRename(mm->params,"x-evpayload",msg,"evpayload");
	copyRename(mm->params,"x-format",msg,"format");
	copyRename(mm->params,"x-direction",msg,"direction");
	copyRename(mm->params,"x-ssrc",msg,"ssrc");
	copyRename(mm->params,"x-drillhole",msg,"drillhole");
	copyRename(mm->params,"x-autoaddr",msg,"autoaddr");
	copyRename(mm->params,"x-anyssrc",msg,"anyssrc");
    }
    mm = sendSync(mm,ep->address);
    if (!mm)
	return false;
    if (m_connId.null())
	m_connId = mm->params.getParam("i");
    if (m_connId.null())
	return false;
    copyRename(msg,"localip",mm->params,"x-localip");
    copyRename(msg,"localport",mm->params,"x-localport");
    msg.setParam("rtpid",id());
    return true;
}

// Delete remote connection if any
void MGCPWrapper::clearConn()
{
    if (m_connId.null() || !s_endpoint)
	return;
    MGCPEpInfo* ep = s_endpoint->find(m_connEp);
    if (!ep)
	return;
    MGCPMessage* mm = new MGCPMessage(s_engine,"DLCX",ep->toString());
    addParams(mm);
    s_engine->sendCommand(mm,ep->address);
}

// Populate a MGCP message with basic identification parameters
void MGCPWrapper::addParams(MGCPMessage* mm)
{
    if (!mm)
	return;
    if (m_connId)
	mm->params.addParam("I",m_connId);
    if (m_master) {
	String callId;
	callId.hexify((void*)m_master.c_str(),m_master.length(),0,true);
	mm->params.addParam("C",callId);
    }
}

// Send a MGCP message, wait for an answer and return it
RefPointer<MGCPMessage> MGCPWrapper::sendSync(MGCPMessage* mm, const SocketAddr& address)
{
    while (m_msg) {
	if (Thread::check(false))
	    return 0;
	Thread::msleep(10);
    }
    MGCPTransaction* tr = s_engine->sendCommand(mm,address);
    tr->userData(static_cast<GenObject*>(this));
    m_tr = tr;
    while (m_tr == tr)
	Thread::msleep(10);
    RefPointer<MGCPMessage> tmp = m_msg;
    m_msg = 0;
    if (tmp)
	Debug(&splugin,DebugNote,"MGCPWrapper::sendSync() returning %d '%s' [%p]",
	    tmp->code(),tmp->comment().c_str(),this);
    else
	Debug(&splugin,DebugMild,"MGCPWrapper::sendSync() returning NULL [%p]",this);
    return tmp;
}

// Find a wrapper by Call Endpoint and media type
MGCPWrapper* MGCPWrapper::find(const CallEndpoint* conn, const String& media)
{
    if (media.null() || !conn)
	return 0;
    Lock lock(s_mutex);
    ObjList* l = &s_wrappers;
    for (; l; l=l->next()) {
	const MGCPWrapper *p = static_cast<const MGCPWrapper *>(l->get());
	if (p && (p->getCall() == conn) && (p->name() == media))
	    return const_cast<MGCPWrapper *>(p);
    }
    return 0;
}

// Find a wrapper by its local ID
MGCPWrapper* MGCPWrapper::find(const String& id)
{
    if (id.null())
	return 0;
    Lock lock(s_mutex);
    ObjList* l = &s_wrappers;
    for (; l; l=l->next()) {
	const MGCPWrapper *p = static_cast<const MGCPWrapper *>(l->get());
	if (p && (p->id() == id))
	    return const_cast<MGCPWrapper *>(p);
    }
    return 0;
}

// Find a wrapper by its Notify-ID
MGCPWrapper* MGCPWrapper::findNotify(const String& id)
{
    if (id.null())
	return 0;
    Lock lock(s_mutex);
    ObjList* l = &s_wrappers;
    for (; l; l=l->next()) {
	MGCPWrapper* w = static_cast<MGCPWrapper *>(l->get());
	if (w && (w->ntfyId() == id))
	    return w;
    }
    return 0;
}

// Send a DTMF as a sequence of package D events
bool MGCPWrapper::sendDTMF(const String& tones)
{
    Debug(&splugin,DebugStub,"MGCPWrapper::sendDTMF('%s') [%p]",
	tones.c_str(),this);
    MGCPEpInfo* ep = s_endpoint->find(m_connEp);
    if (!ep)
	return false;
    MGCPMessage* mm = new MGCPMessage(s_engine,"NTFY",ep->toString());
    addParams(mm);
    String tmp;
    for (unsigned int i = 0; i < tones.length(); i++) {
	if (tmp)
	    tmp << ",";
	tmp << "D/" << tones.at(i);
    }
    mm->params.setParam("O",tmp);
    return s_engine->sendCommand(mm,ep->address) != 0;
}

void MGCPWrapper::gotDTMF(char tone)
{
    Debug(&splugin,DebugInfo,"MGCPWrapper::gotDTMF('%c') [%p]",tone,this);
    if (m_master.null())
	return;
    char buf[2];
    buf[0] = tone;
    buf[1] = 0;
    Message *m = new Message("chan.masquerade");
    m->addParam("id",m_master);
    m->addParam("message","chan.dtmf");
    m->addParam("text",buf);
    m->addParam("detected","mgcp");
    Engine::enqueue(m);
}

// Perform remote bridging if two MGCP endpoints are connected locally
bool MGCPWrapper::nativeConnect(DataEndpoint* peer)
{
    MGCPWrapper* other = YOBJECT(MGCPWrapper,peer);
    if (!other)
	return false;
    // check if the other connection is using same endpoint
    if (other->connEp() != m_connEp)
	return false;
    if (other->connId().null()) {
	Debug(&splugin,DebugWarn,"Not bridging to uninitialized %p [%p]",other,this);
	return false;
    }
    Debug(&splugin,DebugStub,"Native bridging to %p [%p]",other,this);
    MGCPEpInfo* ep = s_endpoint->find(m_connEp);
    if (!ep)
	return false;
    MGCPMessage* mm = new MGCPMessage(s_engine,"MDCX",ep->toString());
    addParams(mm);
    mm->params.setParam("Z2",other->connId());
    return s_engine->sendCommand(mm,ep->address) != 0;
}


// Called by the factory to create MGCP spans
void* MGCPSpan::create(const String& type, const NamedList& name)
{
    if (type != "voice")
	return 0;
    const String* spanName = name.getParam(type);
    if (null(spanName) || !s_endpoint)
	return 0;
    MGCPEpInfo* ep = s_endpoint->findAlias(*spanName);
    if (!ep)
	return 0;
    MGCPSpan* span = new MGCPSpan(name,spanName->c_str(),*ep);
    if (span->init(name))
	return span;
    TelEngine::destruct(span);
    return 0;
}

// Find a span by its Notify-ID
MGCPSpan* MGCPSpan::findNotify(const String& id)
{
    if (id.null())
	return 0;
    Lock lock(s_mutex);
    ObjList* l = &s_spans;
    for (; l; l=l->next()) {
	MGCPSpan* s = static_cast<MGCPSpan*>(l->get());
	if (s && (s->ntfyId() == id))
	    return s;
    }
    return 0;
}

MGCPSpan::MGCPSpan(const NamedList& params, const char* name, const MGCPEpInfo& ep)
    : SignallingCircuitSpan(params.getValue("debugname",name),
       static_cast<SignallingCircuitGroup*>(params.getObject("SignallingCircuitGroup"))),
      m_circuits(0), m_count(1), m_epId(ep), m_operational(false)
{
    Debug(&splugin,DebugAll,"MGCPSpan::MGCPSpan(%p,'%s') [%p]",
	&params,name,this);
    u_int32_t ntfy = (u_int32_t)::random();
    m_notify.hexify(&ntfy,sizeof(ntfy));
    s_mutex.lock();
    s_spans.append(this);
    s_mutex.unlock();
}

MGCPSpan::~MGCPSpan()
{
    Debug(&splugin,DebugAll,"MGCPSpan::~MGCPSpan() '%s' [%p]",
	id().c_str(),this);
    s_mutex.lock();
    s_spans.remove(this,false);
    s_mutex.unlock();
    clearCircuits();
}

// Clear all the circuits in the span
void MGCPSpan::clearCircuits()
{
    MGCPCircuit** circuits = m_circuits;
    m_circuits = 0;
    if (!circuits)
	return;
    for (unsigned int i = 0; i < m_count; i++)
	TelEngine::destruct(circuits[i]);
    delete[] circuits;
}

// Initialize the circuits span
bool MGCPSpan::init(const NamedList& params)
{
    clearCircuits();
    int cicStart = params.getIntValue("start");
    if (cicStart < 0)
	return false;

    if (!m_count)
	return false;
    m_circuits = new MGCPCircuit*[m_count];
    unsigned int i;
    for (i = 0; i < m_count; i++)
	m_circuits[i] = 0;
    bool ok = true;
    for (i = 0; i < m_count; i++) {
	MGCPCircuit* circuit = new MGCPCircuit(cicStart + i,this);
	m_circuits[i] = circuit;
	if (!m_group->insert(circuit)) {
	    Debug(m_group,DebugNote,"MGCPSpan('%s'). Failed to create/insert circuit %u. Rollback [%p]",
		id().safe(),cicStart + i,this);
	    clearCircuits();
	    ok = false;
	    break;
	}
	circuit->ref();
    }
    if (ok)
	const_cast<NamedList&>(params).setParam("chans",String(m_count));
    return ok;
}

// Set the operational state
void MGCPSpan::operational(bool active)
{
    if (active == m_operational)
	return;
    Debug(&splugin,DebugNote,"MGCPSpan '%s' is%s operational [%p]",
	id().c_str(),(active ? "" : " not"),this);
    m_operational = active;
}

// Check if this span matches an endpoint ID
bool MGCPSpan::matchEndpoint(const MGCPEndpointId& ep)
{
    if (ep.port() != m_epId.port())
	return false;
    if (ep.host() |= m_epId.host())
	return false;
    if (ep.user() &= m_epId.user())
	return true;
    if (ep.user() == "*")
	return true;
    return false;
}

// Get the circuit associated to a specific endpoint and request Id
MGCPCircuit* MGCPSpan::findCircuit(const String& epId, const String& rqId) const
{
    if (!(m_count && m_circuits))
	return 0;
    int sep = epId.find('@');
    if (sep <= 0)
	return 0;
    String user = epId.substr(0,sep);
    bool localId = (rqId != "0") && !rqId.null();
    for (unsigned int i = 0; i < m_count; i++) {
	MGCPCircuit* circuit = m_circuits[i];
	if (!circuit)
	    continue;
	if (localId && (circuit->ntfyId() != rqId))
	    continue;
	return circuit;
    }
    return 0;
}

// Process incoming events for this span
bool MGCPSpan::processEvent(MGCPTransaction* tr, MGCPMessage* mm)
{
    Debug(&splugin,DebugStub,"MGCPSpan::processEvent(%p,%p) '%s' [%p]",
	tr,mm,mm->name().c_str(),this);

    if (mm->name() == "NTFY") {
	const String* rqId = mm->params.getParam("x");
	if (null(rqId))
	    return false;
	const String* event = mm->params.getParam("o");
	if (c_str(event) && processNotify(tr,mm,*event,*rqId)) {
	    tr->setResponse(200);
	    return true;
	}
    }

    return false;
}

// Process incoming notify events for this span
bool MGCPSpan::processNotify(MGCPTransaction* tr, MGCPMessage* mm, const String& event, const String& requestId)
{
    Debug(&splugin,DebugStub,"MGCPSpan::processNotify(%p,%p,'%s','%s') [%p]",
	tr,mm,event.c_str(),requestId.c_str(),this);

    if (event.null())
	return false;
    else if (event.find(',') >= 0) {
	// Multiple events
	ObjList* l = event.split(',',false);
	bool ok = false;
	for (ObjList* p = l->skipNull(); p; p = p->skipNext())
	    ok = processNotify(tr,mm,p->get()->toString(),requestId) || ok;
	delete l;
	return ok;
    }
    else {
	MGCPCircuit* circuit = findCircuit(mm->endpointId(),requestId);
	return circuit && circuit->processNotify(event);
    }
#if 0
    else if (event &= "L/hd") {
	// Line off-hook
    }
    else if (event &= "L/hu") {
	// Line on-hook
    }
    else
	return false;
    return true;
#endif
}

// Process gateway restart events for this span
bool MGCPSpan::processRestart(MGCPTransaction* tr, MGCPMessage* mm, const String& method)
{
    Debug(&splugin,DebugStub,"MGCPSpan::processRestart(%p,%p,'%s') [%p]",
	tr,mm,method.c_str(),this);
    if (method &= "X-KeepAlive")
	return true;
    if (method &= "disconnected") {
	operational(true);
    }
    else if (method &= "graceful")
	operational(false);
    else if (method &= "cancel-graceful")
	operational(true);
    else {
	operational(false);
    }
    return true;
}


MGCPCircuit::MGCPCircuit(unsigned int code, MGCPSpan* span)
    : SignallingCircuit(RTP,code,Missing,span->group(),span),
      m_statusReq(Missing)
{
    Debug(&splugin,DebugAll,"MGCPCircuit::MGCPCircuit(%u,%p) [%p]",
	code,span,this);
    u_int32_t cic = code;
    m_notify.hexify(&cic,sizeof(cic));
    m_notify = span->ntfyId() + m_notify;
}

MGCPCircuit::~MGCPCircuit()
{
    Debug(&splugin,DebugAll,"MGCPCircuit::~MGCPCircuit() %u [%p]",
	code(),this);
}

// Circuit status change request
bool MGCPCircuit::status(Status newStat, bool sync)
{
    Debug(&splugin,DebugInfo,"MGCPCircuit::status(%s,%s) [%p]",
	lookupStatus(newStat),String::boolText(sync),this);
    if ((newStat == m_statusReq) && ((SignallingCircuit::status() == newStat) || !sync))
	return true;
    if (!static_cast<MGCPSpan*>(span())->operational()) {
	if (newStat >= Idle)
	    return false;
    }
    m_statusReq = newStat;
    // !!!
    return SignallingCircuit::status(newStat,sync);
}

// Process notifications for this circuit
bool MGCPCircuit::processNotify(const String& event)
{
    Debug(&splugin,DebugStub,"MGCPCircuit::processNotify('%s') %u [%p]",
	event.c_str(),code(),this);
    return false;
}


// Handler for chan.rtp messages - one per media type
bool RtpHandler::received(Message& msg)
{
    // refuse calls from a MGCP-GW
    if (!msg.getBoolValue("mgcp_allowed",true))
	return false;
    String trans = msg.getValue("transport");
    if (trans && !trans.startsWith("RTP/"))
	return false;
    Debug(&splugin,DebugAll,"RTP message received");

    MGCPWrapper* w = 0;
    const char* media = msg.getValue("media","audio");
    CallEndpoint *ch = static_cast<CallEndpoint*>(msg.userData());
    if (ch) {
	w = MGCPWrapper::find(ch,media);
	if (w)
	    Debug(&splugin,DebugAll,"Wrapper %p found by CallEndpoint",w);
    }
    if (!w) {
	w = MGCPWrapper::find(msg.getValue("rtpid"));
	if (w)
	    Debug(&splugin,DebugAll,"Wrapper %p found by ID",w);
    }
    if (!(ch || w)) {
	Debug(&splugin,DebugWarn,"Neither call channel nor MGCP wrapper found!");
	return false;
    }

    if (w)
	return w->rtpMessage(msg);

    if (ch)
	ch->clearEndpoint(media);
    w = new MGCPWrapper(ch,media,msg);
    if (!w->rtpMessage(msg))
	return false;
    if (ch && ch->getPeer())
	w->connect(ch->getPeer()->getEndpoint(media));

    return true;
}


// Handler for chan.sdp messages - one message for all media at once
bool SdpHandler::received(Message& msg)
{
    // refuse calls from a MGCP-GW
    if (!msg.getBoolValue("mgcp_allowed",true))
	return false;
    Debug(&splugin,DebugAll,"SDP message received");

    return false;
}


// Handler for chan.dtmf messages, forwards them to the remote endpoint
bool DTMFHandler::received(Message& msg)
{
    String targetid(msg.getValue("targetid"));
    if (targetid.null())
	return false;
    String text(msg.getValue("text"));
    if (text.null())
	return false;
    MGCPWrapper* wrap = MGCPWrapper::find(targetid);
    return wrap && wrap->sendDTMF(text);
}


MGCPPlugin::MGCPPlugin()
    : Module("mgcpca","misc")
{
    Output("Loaded module MGCP-CA");
}

MGCPPlugin::~MGCPPlugin()
{
    Output("Unloading module MGCP-CA");
    s_wrappers.clear();
    s_spans.clear();
    delete s_engine;
}

void MGCPPlugin::statusParams(String& str)
{
    s_mutex.lock();
    str.append("spans=",",") << s_spans.count();
    str.append("chans=",",") << s_wrappers.count();
    s_mutex.unlock();
}

void MGCPPlugin::statusDetail(String& str)
{
    s_mutex.lock();
    ObjList* l = s_wrappers.skipNull();
    for (; l; l=l->skipNext()) {
	MGCPWrapper* w = static_cast<MGCPWrapper*>(l->get());
        str.append(w->id(),",") << "=" << w->callId();
    }
    s_mutex.unlock();
}

void MGCPPlugin::initialize()
{
    Output("Initializing module MGCP Call Agent");
    Configuration cfg(Engine::configFile("mgcpca"));
    setup();
    NamedList* engSect = cfg.getSection("engine");
    if (s_engine && engSect)
	s_engine->initialize(*engSect);
    while (!s_engine) {
	if (!(engSect && engSect->getBoolValue("enabled",true)))
	    break;
	int n = cfg.sections();
	for (int i = 0; i < n; i++) {
	    NamedList* sect = cfg.getSection(i);
	    if (!sect)
		continue;
	    String name(*sect);
	    if (name.startSkip("gw") && name) {
		const char* host = sect->getValue("host");
		if (!host)
		    continue;
		if (!s_engine) {
		    s_engine = new YMGCPEngine(engSect);
		    s_engine->debugChain(this);
		    s_endpoint = new MGCPEndpoint(
			s_engine,
			cfg.getValue("endpoint","user","yate"),
			cfg.getValue("endpoint","host",s_engine->address().host()),
			cfg.getIntValue("endpoint","port")
		    );
		}
		MGCPEpInfo* ep = s_endpoint->append(
		    sect->getValue("user",name),
		    host,
		    sect->getIntValue("port",0)
		);
		if (ep) {
		    ep->alias = sect->getValue("name",name);
		    if (s_defaultEp.null() || sect->getBoolValue("default"))
			s_defaultEp = ep->toString();
		}
		else
		    Debug(this,DebugWarn,"Could not set endpoint for gateway '%s'",
			name.c_str());
	    }
	}
	if (!s_engine) {
	    Debug(this,DebugAll,"No gateways defined so module not initialized.");
	    break;
	}
	Debug(this,DebugNote,"Default remote endpoint: '%s'",s_defaultEp.c_str());
	int prio = cfg.getIntValue("general","priority",80);
	if (prio > 0) {
	    Engine::install(new RtpHandler(prio));
	    Engine::install(new SdpHandler(prio));
	    Engine::install(new DTMFHandler);
	}
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */

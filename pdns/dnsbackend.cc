/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2005  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation

    Additionally, the license of this program contains a special
    exception which allows to distribute the program in binary form when
    it is linked against OpenSSL.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "utility.hh"
#include "dnsbackend.hh"
#include "arguments.hh"
#include "ueberbackend.hh"
#include "logger.hh"

#include <sys/types.h>
#include "pdns/packetcache.hh"
#include "dnspacket.hh"
#include "dns.hh"

bool DNSBackend::getAuth(DNSPacket *p, SOAData *sd, const DNSName &target, const int best_match_len)
{
  bool found=false;
  DNSName subdomain(target);
  do {
    if( best_match_len >= (int)subdomain.toString().length() )
      break;

    if( this->getSOA( subdomain, *sd, p ) ) {
      sd->qname = subdomain;

      if(p->qtype.getCode() == QType::DS && subdomain==target) {
        // Found authoritative zone but look for parent zone with 'DS' record.
        found=true;
      } else
        return true;
    }
  }
  while( subdomain.chopOff() );   // 'www.powerdns.org' -> 'powerdns.org' -> 'org' -> ''

  return found;
}

void DNSBackend::setArgPrefix(const string &prefix)
{
  d_prefix=prefix;
}

bool DNSBackend::mustDo(const string &key)
{
  return arg().mustDo(d_prefix+"-"+key);
}

const string &DNSBackend::getArg(const string &key)
{
  return arg()[d_prefix+"-"+key];
}

int DNSBackend::getArgAsNum(const string &key)
{
  return arg().asNum(d_prefix+"-"+key);
}

void BackendFactory::declare(const string &suffix, const string &param, const string &help, const string &value)
{
  string fullname=d_name+suffix+"-"+param;
  arg().set(fullname,help)=value;
}

const string &BackendFactory::getName() const
{
  return d_name;
}

BackendMakerClass &BackendMakers()
{
  static BackendMakerClass bmc;
  return bmc;
}

void BackendMakerClass::report(BackendFactory *bf)
{
  d_repository[bf->getName()]=bf;
}


vector<string> BackendMakerClass::getModules()
{
  load_all();
  vector<string> ret;
  //  copy(d_repository.begin(), d_repository.end(),back_inserter(ret));
  for(d_repository_t::const_iterator i=d_repository.begin();i!=d_repository.end();++i)
    ret.push_back(i->first);
  return ret;
}

void BackendMakerClass::load_all()
{
  // TODO: Implement this?
  DIR *dir=opendir(arg()["module-dir"].c_str());
  if(!dir) {
    L<<Logger::Error<<"Unable to open module directory '"<<arg()["module-dir"]<<"'"<<endl;
    return;
  }
  struct dirent *entry;
  while((entry=readdir(dir))) {
    if(!strncmp(entry->d_name,"lib",3) &&
       strlen(entry->d_name)>13 &&
       !strcmp(entry->d_name+strlen(entry->d_name)-10,"backend.so"))
      load(entry->d_name);
  }
  closedir(dir);
}

void BackendMakerClass::load(const string &module)
{
  int res;

  if(module.find(".")==string::npos)
    res=UeberBackend::loadmodule(arg()["module-dir"]+"/lib"+module+"backend.so");
  else if(module[0]=='/' || (module[0]=='.' && module[1]=='/') || (module[0]=='.' && module[1]=='.'))    // absolute or current path
    res=UeberBackend::loadmodule(module);
  else
    res=UeberBackend::loadmodule(arg()["module-dir"]+"/"+module);

  if(res==false) {
    L<<Logger::Error<<"DNSBackend unable to load module in "<<module<<endl;
    exit(1);
  }
}

void BackendMakerClass::launch(const string &instr)
{
  //    if(instr.empty())
  // throw ArgException("Not launching any backends - nameserver won't function");

  vector<string> parts;
  stringtok(parts,instr,", ");

  for(vector<string>::const_iterator i=parts.begin();i!=parts.end();++i) {
    const string &part=*i;

    string module, name;
    vector<string>pparts;
    stringtok(pparts,part,": ");
    module=pparts[0];
    if(pparts.size()>1)
      name="-"+pparts[1];

    if(d_repository.find(module)==d_repository.end()) {
      // this is *so* userfriendly
      load(module);
      if(d_repository.find(module)==d_repository.end())
        throw ArgException("Trying to launch unknown backend '"+module+"'");
    }
    d_repository[module]->declareArguments(name);
    d_instances.push_back(make_pair(module,name));
  }
}

int BackendMakerClass::numLauncheable()
{
  return d_instances.size();
}

vector<DNSBackend *>BackendMakerClass::all(bool metadataOnly)
{
  vector<DNSBackend *>ret;
  if(d_instances.empty())
    throw PDNSException("No database backends configured for launch, unable to function");

  try {
    for(vector<pair<string,string> >::const_iterator i=d_instances.begin();i!=d_instances.end();++i) {
      DNSBackend *made;
      if(metadataOnly)
        made = d_repository[i->first]->makeMetadataOnly(i->second);
      else
        made = d_repository[i->first]->make(i->second);
      if(!made)
        throw PDNSException("Unable to launch backend '"+i->first+"'");

      ret.push_back(made);
    }
  }
  catch(PDNSException &ae) {
    L<<Logger::Error<<"Caught an exception instantiating a backend: "<<ae.reason<<endl;
    L<<Logger::Error<<"Cleaning up"<<endl;
    for(vector<DNSBackend *>::const_iterator i=ret.begin();i!=ret.end();++i)
      delete *i;
    throw;
  } catch(...) {
    // and cleanup
    L<<Logger::Error<<"Caught an exception instantiating a backend, cleaning up"<<endl;
    for(vector<DNSBackend *>::const_iterator i=ret.begin();i!=ret.end();++i)
      delete *i;
    throw;
  }

  return ret;
}

/** getSOA() is a function that is called to get the SOA of a domain. Callers should ONLY
    use getSOA() and not perform a lookup() themselves as backends may decide to special case
    the SOA record.

    Returns false if there is definitely no SOA for the domain. May throw a DBException
    to indicate that the backend is currently unable to supply an answer.

    WARNING: This function *may* fill out the db attribute of the SOAData, but then again,
    it may not! If you find a zero in there, you may have been handed a non-live and cached
    answer, in which case you need to perform a getDomainInfo call!

    \param domain Domain we want to get the SOA details of
    \param sd SOAData which is filled with the SOA details
*/
bool DNSBackend::getSOA(const DNSName &domain, SOAData &sd, DNSPacket *p)
{
  this->lookup(QType(QType::SOA),domain,p);

  DNSResourceRecord rr;
  rr.auth = true;

  int hits=0;

  while(this->get(rr)) {
    if (rr.qtype != QType::SOA) throw PDNSException("Got non-SOA record when asking for SOA");
    hits++;
    fillSOAData(rr.content, sd);
    sd.domain_id=rr.domain_id;
    sd.ttl=rr.ttl;
    sd.scopeMask = rr.scopeMask;
  }

  if(!hits)
    return false;
  sd.qname = domain;
  if(!sd.nameserver.countLabels())
    sd.nameserver= DNSName(arg()["default-soa-name"]);

  if(!sd.hostmaster.countLabels()) {
    if (!arg().isEmpty("default-soa-mail")) {
      sd.hostmaster= DNSName(arg()["default-soa-mail"]);
      // attodot(sd.hostmaster); FIXME400
    }
    else
      sd.hostmaster=DNSName("hostmaster")+domain;
  }

  if(!sd.serial) { // magic time!
    DLOG(L<<Logger::Warning<<"Doing SOA serial number autocalculation for "<<rr.qname<<endl);

    time_t serial;
    if (calculateSOASerial(domain, sd, serial)) {
      sd.serial = serial;
      //DLOG(L<<"autocalculated soa serialnumber for "<<rr.qname<<" is "<<newest<<endl);
    } else {
      DLOG(L<<"soa serialnumber calculation failed for "<<rr.qname<<endl);
    }

  }
  sd.db=this;
  return true;
}

bool DNSBackend::getBeforeAndAfterNames(uint32_t id, const DNSName& zonename, const DNSName& qname, DNSName& before, DNSName& after)
{
  // FIXME400 FIXME400 FIXME400
  // string lcqname=toLower(qname); FIXME400 tolower?
  // string lczonename=toLower(zonename); FIXME400 tolower?
  // lcqname=makeRelative(lcqname, lczonename);
  DNSName lczonename = DNSName(toLower(zonename.toString()));
  // lcqname=labelReverse(lcqname);
  DNSName dnc;
  string relqname, sbefore, safter;
  relqname=labelReverse(makeRelative(qname.toString(), zonename.toString()));
  //sbefore = before.toString();
  //safter = after.toString();
  bool ret = this->getBeforeAndAfterNamesAbsolute(id, relqname, dnc, sbefore, safter);
  before = DNSName(labelReverse(sbefore)) + lczonename;
  after = DNSName(labelReverse(safter)) + lczonename;

  // before=dotConcat(labelReverse(before), lczonename); FIXME400
  // after=dotConcat(labelReverse(after), lczonename); FIXME400
  return ret;
}

/**
 * Calculates a SOA serial for the zone and stores it in the third
 * argument. Returns false if calculation is not possible for some
 * reason (in this case, the third argument is not inspected). If it
 * returns true, the value returned in the third argument will be set
 * as the SOA serial.
 *
 * \param domain The name of the domain
 * \param sd Information about the SOA record already available
 * \param serial Output parameter. Only inspected when we return true
 */
bool DNSBackend::calculateSOASerial(const DNSName& domain, const SOAData& sd, time_t& serial)
{
    // we do this by listing the domain and taking the maximum last modified timestamp

    DNSResourceRecord i;
    time_t newest=0;

    if(!(this->list(domain, sd.domain_id))) {
      DLOG(L<<Logger::Warning<<"Backend error trying to determine magic serial number of zone '"<<domain<<"'"<<endl);
      return false;
    }

    while(this->get(i)) {
      if(i.last_modified>newest)
        newest=i.last_modified;
    }

    serial=newest;

    return true;
}

/* This is a subclass of DNSBackend that, assuming you have your zones reversed
 * and stored in an ordered fashion, will be able to look up SOA's much quicker
 * than the DNSBackend code. The normal case for a SOA that exists is 1 backend
 * query no matter how much the depth (although if there are sub-SOA's then
 * this could require one or two more queries). The normal case for an SOA that
 * does not exist is 2 or 3 queries depending on the system, although this will
 * be reduced if the negative cache is active.
 *
 * The subclass MUST implement bool getAuthZone(string &reversed_zone_name)
 * which, given a reversed zone name will return false if there was some sort
 * of error (eg no record found as top of database was hit, lookup issues),
 * otherwise returns true and sets reversed_zone_name to be the exact entry
 * found, otherwise the entry directly preceding where it would be.
 *
 * The subclass MUST implement getAuthData( const string &rev_zone_name, SOAData *soa )
 * which is basically the same as getSOA() but is called with the reversed zone name
 */
enum {
    GET_AUTH_NEG_DONTCACHE, // not found but don't cache this fact
    GET_AUTH_NEG_CACHE,     // not found and negcache this
    GET_AUTH_SUCCESS,       // entry found
};

#undef PC
extern PacketCache PC;

#if 0
#undef DLOG
#define DLOG(x) x
#endif

// XXX DNSName pain, should be DNSName native. 
static bool add_to_negcache( const string &zone ) {
    static int negqueryttl=::arg().asNum("negquery-cache-ttl");
    // add the zone to the negative query cache and return false
    if(negqueryttl) {
        DLOG(L<<Logger::Error<<"Adding to neg qcache: " << zone<<endl);
        PC.insert(DNSName(zone), QType(QType::SOA), PacketCache::QUERYCACHE, "", negqueryttl, 0);
    }
    return false;
}

// XXX DNSName Pain, this should be DNSName native!                     vvvvvvvvvvvvvv
inline int DNSReversedBackend::_getAuth(DNSPacket *p, SOAData *soa, const string &inZone, const string &querykey, const int best_match_len) {
    static int negqueryttl=::arg().asNum("negquery-cache-ttl");

    DLOG(L<<Logger::Error<<"SOA Query: " <<querykey<<endl);

    /* Got a match from a previous backend that was longer than this - no need
     * to continue. This is something of an optimization as we would hit the
     * similar test below in any cases that this was hit, although we would run
     * the risk of something being added to the neg-querycache that may
     * interfear with future queries
     */
    if( best_match_len >= (int)querykey.length() ) {
        DLOG(L<<Logger::Error<<"Best match was better from a different client"<<endl);
        return GET_AUTH_NEG_DONTCACHE;
    }

    /* Look up in the negative querycache to see if we have already tried and
     * failed to look up this zone */
    if( negqueryttl ) {
        string content;
        bool ret = PC.getEntry( DNSName(inZone), QType(QType::SOA), PacketCache::QUERYCACHE, content, 0 );
        if( ret && content.empty() ) {
            DLOG(L<<Logger::Error<<"Found in neg qcache: " << inZone << ":" << content << ":" << ret << ":"<<endl);
            return GET_AUTH_NEG_DONTCACHE;
        }
    }

    /* Find the SOA entry on- or before- the position that we want in the b-tree */
    string foundkey = querykey;
    if( !getAuthZone( foundkey ) )
        return GET_AUTH_NEG_CACHE;

    DLOG(L<<Logger::Error<<"Queried: " << querykey << " and found record: " <<foundkey<<endl);

    // Got a match from a previous backend that was longer than this - no need
    // to continue.
    if( best_match_len && best_match_len >= (int) foundkey.length() ) {
        DLOG(L<<Logger::Error<<"Best match was better from a different client"<<endl);
        return GET_AUTH_NEG_DONTCACHE;
    }

    // Found record successfully now, fill in the data.
    if( getAuthData( *soa, p ) ) {
        /* all the keys are reversed. rather than reversing them again it is
         * presumably quicker to just substring the zone down to size */
      soa->qname = DNSName(inZone.substr( inZone.length() - foundkey.length(), string::npos ));

      DLOG(L<<Logger::Error<<"Successfully got record: " <<foundkey << " : " << querykey.substr( 0, foundkey.length() ) << " : " << soa->qname<<endl);

      return GET_AUTH_SUCCESS;
    }

    return GET_AUTH_NEG_CACHE;
}

bool DNSReversedBackend::getAuth(DNSPacket *p, SOAData *soa, const DNSName &inZone, const int best_match_len) {
    // Reverse the lowercased query string
    string zone = toLower(inZone.toStringNoDot());
    string querykey = labelReverse(zone);

    int ret = _getAuth( p, soa, inZone.toStringNoDot(), querykey, best_match_len );

    /* If this is disabled then we would just cache the tree structure not the
     * leaves which should give the best performance and a nice small negcache
     * size
     */
    if( ret == GET_AUTH_NEG_CACHE )
      add_to_negcache( inZone.toStringNoDot() );

    return ret == GET_AUTH_SUCCESS;
}

/* getAuthData() is very similar to getSOA() so implement a default getSOA
 * based on that. This will only be called very occasionally for example during
 * an AXFR */
bool DNSReversedBackend::_getSOA(const string &querykey, SOAData &soa, DNSPacket *p)
{
    string searchkey( querykey );

    if( !getAuthZone( searchkey ) )
        return false;

    DLOG(L<<Logger::Error<<"search key " << searchkey << " query key " << querykey<<endl);

    if( querykey.compare( searchkey ) != 0 )
        return false;

    return getAuthData( soa, p );
}

bool DNSReversedBackend::getSOA(const DNSName &inZone, SOAData &soa, DNSPacket *p)
{
    // prepare the query string
    string zone = toLower( inZone.toStringNoDot() );
    string querykey = labelReverse( zone );

    if( !_getSOA( querykey, soa, p ) )
        return false;

    soa.qname = inZone;
    return true;
}

void fillSOAData(const string &content, SOAData &data)
{
  // content consists of fields separated by spaces:
  //  nameservername hostmaster serial-number [refresh [retry [expire [ minimum] ] ] ]

  // fill out data with some plausible defaults:
  // 10800 3600 604800 3600
  vector<string>parts;
  stringtok(parts,content);
  int pleft=parts.size();

  //  cout<<"'"<<content<<"'"<<endl;

  if(pleft)
    data.nameserver=DNSName(parts[0]);

  if(pleft>1) 
    data.hostmaster=DNSName(attodot(parts[1])); // ahu@ds9a.nl -> ahu.ds9a.nl, piet.puk@ds9a.nl -> piet\.puk.ds9a.nl

  data.serial = pleft > 2 ? pdns_strtoui(parts[2].c_str(), NULL, 10) : 0;
  if (data.serial == UINT_MAX && errno == ERANGE) throw PDNSException("serial number too large in '"+parts[2]+"'");

  data.refresh = pleft > 3 ? atoi(parts[3].c_str())
        : ::arg().asNum("soa-refresh-default");

  data.retry = pleft > 4 ? atoi(parts[4].c_str())
        : ::arg().asNum("soa-retry-default");

  data.expire = pleft > 5 ? atoi(parts[5].c_str())
        : ::arg().asNum("soa-expire-default");

  data.default_ttl = pleft > 6 ?atoi(parts[6].c_str())
        : ::arg().asNum("soa-minimum-ttl");
}

string serializeSOAData(const SOAData &d)
{
  ostringstream o;
  //  nameservername hostmaster serial-number [refresh [retry [expire [ minimum] ] ] ]
  o<<d.nameserver.toString()<<" "<< d.hostmaster.toString() <<" "<< d.serial <<" "<< d.refresh << " "<< d.retry << " "<< d.expire << " "<< d.default_ttl;

  return o.str();
}

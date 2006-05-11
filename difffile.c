/*
 * difffile.c - DIFF file handling source code. Read and write diff files.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "difffile.h"
#include "util.h"
#include "packet.h"
#include "rdata.h"

static int 
write_32(FILE *out, uint32_t val)
{
	val = htonl(val);
	return write_data(out, &val, sizeof(val));
}

static int 
write_16(FILE *out, uint16_t val)
{
	val = htons(val);
	return write_data(out, &val, sizeof(val));
}

static int 
write_8(FILE *out, uint8_t val)
{
	return write_data(out, &val, sizeof(val));
}

static int 
write_str(FILE *out, const char* str)
{
	uint32_t len = strlen(str);
	if(!write_32(out, len)) 
		return 0;
	return write_data(out, str, len);
}

void 
diff_write_packet(const char* zone, uint32_t new_serial, uint16_t id, 
	uint32_t seq_nr, uint8_t* data, size_t len, nsd_options_t* opt)
{
	const char* filename = DIFFFILE;
	FILE *df;
	uint32_t file_len = sizeof(uint32_t) + strlen(zone) + 
		sizeof(new_serial) + sizeof(id) + sizeof(seq_nr) + len;

	if(opt->difffile) 
		filename = opt->difffile;

	df = fopen(filename, "a");
	if(!df) {
		log_msg(LOG_ERR, "could not open file %s for append: %s",
			filename, strerror(errno));
		return;
	}

	if(!write_32(df, DIFF_PART_IXFR) ||
		!write_32(df, file_len) ||
		!write_str(df, zone) ||
		!write_32(df, new_serial) ||
		!write_16(df, id) ||
		!write_32(df, seq_nr) ||
		!write_data(df, data, len) ||
		!write_32(df, file_len)) 
	{
		log_msg(LOG_ERR, "could not write to file %s: %s",
			filename, strerror(errno));
	}
	fflush(df);
	fclose(df);
}

void 
diff_write_commit(const char* zone, uint32_t old_serial,
	uint32_t new_serial, uint16_t id, uint32_t num_parts,
	uint8_t commit, const char* log_str, 
	nsd_options_t* opt)
{
	const char* filename = DIFFFILE;
	FILE *df;
	uint32_t len;
	if(opt->difffile) 
		filename = opt->difffile;

	df = fopen(filename, "a");
	if(!df) {
		log_msg(LOG_ERR, "could not open file %s for append: %s",
			filename, strerror(errno));
		return;
	}

	len = strlen(zone)+sizeof(len) + sizeof(old_serial) + 
		sizeof(new_serial) + sizeof(id) + sizeof(num_parts) +
		sizeof(commit) + strlen(log_str)+sizeof(len);

	if(!write_32(df, DIFF_PART_SURE) ||
		!write_32(df, len) ||
		!write_str(df, zone) ||
		!write_32(df, old_serial) ||
		!write_32(df, new_serial) ||
		!write_16(df, id) ||
		!write_32(df, num_parts) ||
		!write_8(df, commit) ||
		!write_str(df, log_str) ||
		!write_32(df, len)) 
	{
		log_msg(LOG_ERR, "could not write to file %s: %s",
			filename, strerror(errno));
	}
	fflush(df);
	fclose(df);
}

int 
db_crc_different(namedb_type* db)
{
	FILE *fd = fopen(db->filename, "r");
	uint32_t crc_file;
	char buf[NAMEDB_MAGIC_SIZE];
	if(fd == NULL) {
		log_msg(LOG_ERR, "unable to load %s: %s",
			db->filename, strerror(errno));
		return -1;
	}
	
	/* seek to position of CRC, check it and magic no */
	if(fsetpos(fd, &db->crc_pos)==-1) {
		log_msg(LOG_ERR, "unable to fsetpos %s: %s. db changed?",
			db->filename, strerror(errno));
		fclose(fd);
		return -1;
	}

	if(fread(&crc_file, sizeof(crc_file), 1, fd) != 1) {
		log_msg(LOG_ERR, "could not read %s CRC. db changed?", db->filename);
		fclose(fd);
		return -1;
	}
	crc_file = ntohl(crc_file);

	if(fread(buf, sizeof(char), sizeof(buf), fd) != sizeof(buf)
	   || memcmp(buf, NAMEDB_MAGIC, NAMEDB_MAGIC_SIZE) != 0) {
		log_msg(LOG_ERR, "could not read %s magic. db changed?", db->filename);
		fclose(fd);
		return -1;
	}

	fclose(fd);

	if(db->crc == crc_file)
		return 0;
	return 1;
}

static int 
read_32(FILE *in, uint32_t* result)
{
        if (fread(result, sizeof(*result), 1, in) == 1) {
                *result = ntohl(*result);
                return 1;
        } else {
                return 0;
        }
}

static int 
read_16(FILE *in, uint16_t* result)
{
        if (fread(result, sizeof(*result), 1, in) == 1) {
                *result = ntohs(*result);
                return 1;
        } else {
                return 0;
        }
}

static int 
read_8(FILE *in, uint8_t* result)
{
        if (fread(result, sizeof(*result), 1, in) == 1) {
                return 1;
        } else {
                return 0;
        }
}

static int 
read_str(FILE* in, char* buf, size_t len)
{
	uint32_t disklen;
	if(!read_32(in, &disklen)) 
		return 0;
	if(disklen >= len) 
		return 0;
	if(fread(buf, disklen, 1, in) != 1) 
		return 0;
	buf[disklen] = 0;
	return 1;
}

static void 
rrset_delete(domain_type* domain, rrset_type* rrset)
{
	/* find previous */
	rrset_type** pp = &domain->rrsets;
	while(*pp && *pp != rrset) {
		pp = &( (*pp)->next );
	}
	if(!*pp) {
		/* rrset does not exist for domain */
		return;
	}
	*pp = rrset->next;

	log_msg(LOG_INFO, "delete rrset of %s type %s", 
		dname_to_string(domain_dname(domain),0),
		rrtype_to_string(rrset_rrtype(rrset)));

	/* is this a SOA rrset ? */
	if(rrset->zone->soa_rrset == rrset) {
		rrset->zone->soa_rrset = 0;
		rrset->zone->updated = 1;
	}
	if(rrset->zone->ns_rrset == rrset) {
		rrset->zone->ns_rrset = 0;
	}
#ifdef DNSSEC
	if(domain == rrset->zone->apex && rrset_rrtype(rrset) == TYPE_RRSIG) {
		int i;
		for (i = 0; i < rrset->rr_count; ++i) {
			if (rr_rrsig_type_covered(&rrset->rrs[i]) == TYPE_SOA) {
				rrset->zone->is_secure = 0;
				break;
			}
		}
	}
#endif
	/* is the node now an empty node (completely deleted) */
	if(domain->rrsets == 0) {
		domain->is_existing = 0;
	}
	rrset->rr_count = 0;
}

static int 
rdatas_equal(rdata_atom_type *a, rdata_atom_type *b, int num, uint16_t type)
{
	int k;
	for(k = 0; k < num; k++)
	{
		if(rdata_atom_is_domain(type, k)) {
			/* check dname */
			if(dname_compare(domain_dname(a[k].domain),
				domain_dname(b[k].domain))!=0)
				return 0;
		} else {
			/* check length */
			if(a[k].data[0] != b[k].data[0]) 
				return 0;
			/* check data */
			if(memcmp(a[k].data+1, b[k].data+1, a[k].data[0])!=0)
				return 0;
		}
	}
	return 1;
}

static int 
find_rr_num(rrset_type* rrset,
	uint16_t type, uint16_t klass, uint32_t ttl, 
	rdata_atom_type *rdatas, ssize_t rdata_num)
{
	int i;
	for(i=0; i<rrset->rr_count; ++i) {
		if(rrset->rrs[i].ttl == ttl &&
		   rrset->rrs[i].type == type &&
		   rrset->rrs[i].klass == klass &&
		   rrset->rrs[i].rdata_count == rdata_num &&
		   rdatas_equal(rdatas, rrset->rrs[i].rdatas, rdata_num, type))
			return i;
	}
	return -1;
}

static void 
delete_RR(namedb_type* db, const dname_type* dname, 
	uint16_t type, uint16_t klass, uint32_t ttl, 
	buffer_type* packet, size_t rdatalen, zone_type *zone,
	region_type* temp_region)
{
	domain_type *domain;
	rrset_type *rrset;
	domain = domain_table_find(db->domains, dname);
	if(!domain) {
		log_msg(LOG_ERR, "diff: domain %s does not exist", 
			dname_to_string(dname,0));
		return;
	}
	rrset = domain_find_rrset(domain, zone, type);
	if(!rrset) {
		log_msg(LOG_ERR, "diff: rrset %s does not exist", 
			dname_to_string(dname,0));
		return;
	} else {
		/* find the RR in the rrset */
		domain_table_type *temptable;
		rdata_atom_type *rdatas;
		ssize_t rdata_num;
		int rrnum;
		temptable = domain_table_create(temp_region);
		rdata_num = rdata_wireformat_to_rdata_atoms(
			temp_region, temptable, type, rdatalen, packet, &rdatas);
		if(rdata_num == -1) {
			log_msg(LOG_ERR, "diff: bad rdata for %s", 
				dname_to_string(dname,0));
			return;
		}
		rrnum = find_rr_num(rrset, type, klass, ttl, rdatas, rdata_num);
		if(rrnum == -1) {
			log_msg(LOG_ERR, "diff: RR %s does not exist", 
				dname_to_string(dname,0));
			return;
		}
		if(rrset->rr_count == 1) {
			/* delete entire rrset */
			rrset_delete(domain, rrset);
		} else {
			/* swap out the bad RR and decrease the count */
			if(rrnum < rrset->rr_count-1)
				rrset->rrs[rrnum] = rrset->rrs[rrset->rr_count-1];
			memset(&rrset->rrs[rrset->rr_count-1], 0, sizeof(rr_type));
			rrset->rr_count --;
		}
	}
}

static void 
add_RR(namedb_type* db, const dname_type* dname, 
	uint16_t type, uint16_t klass, uint32_t ttl, 
	buffer_type* packet, size_t rdatalen, zone_type *zone)
{
	domain_type* domain;
	rrset_type* rrset;
	domain = domain_table_find(db->domains, dname);
	rdata_atom_type *rdatas;
	rr_type *rrs_old;
	ssize_t rdata_num;
	int rrnum;
	if(!domain) {
		/* create the domain */
		domain = domain_table_insert(db->domains, dname);
	}
	rrset = domain_find_rrset(domain, zone, type);
	if(!rrset) {
		/* create the rrset */
		rrset = region_alloc(db->region, sizeof(rrset_type));
		rrset->zone = zone;
		rrset->rrs = 0;
		rrset->rr_count = 0;
		domain_add_rrset(domain, rrset);
	}

	rdata_num = rdata_wireformat_to_rdata_atoms(
		db->region, db->domains, type, rdatalen, packet, &rdatas);
	if(rdata_num == -1) {
		log_msg(LOG_ERR, "diff: bad rdata for %s", 
			dname_to_string(dname,0));
		return;
	}
	rrnum = find_rr_num(rrset, type, klass, ttl, rdatas, rdata_num);
	if(rrnum != -1) {
		log_msg(LOG_ERR, "diff: RR %s already exists", 
			dname_to_string(dname,0));
		/* ignore already existing RR: lenient accepting of messages */
		return;
	}
	
	/* re-alloc the rrs and add the new */
	rrs_old = rrset->rrs;
	rrset->rrs = region_alloc(db->region, 
		(rrset->rr_count+1) * sizeof(rr_type));
	if(rrs_old)
		memcpy(rrset->rrs, rrs_old, rrset->rr_count * sizeof(rr_type));
	rrset->rr_count ++;

	rrset->rrs[rrset->rr_count - 1].owner = domain;
	rrset->rrs[rrset->rr_count - 1].rdatas = rdatas;
	rrset->rrs[rrset->rr_count - 1].ttl = ttl;
	rrset->rrs[rrset->rr_count - 1].type = type;
	rrset->rrs[rrset->rr_count - 1].klass = klass;
	rrset->rrs[rrset->rr_count - 1].rdata_count = rdata_num;

	/* see if it is a SOA */
	if(domain == zone->apex) {
		if(type == TYPE_SOA) {
			uint32_t soa_minimum;
			zone->soa_rrset = rrset;
			zone->updated = 1;
			/* BUG #103 tweaked SOA ttl value */
			if(zone->soa_nx_rrset == 0) {
				zone->soa_nx_rrset = region_alloc(db->region, 
					sizeof(rrset_type));
				zone->soa_nx_rrset->rr_count = 1;
				zone->soa_nx_rrset->next = 0;
				zone->soa_nx_rrset->zone = zone;
				zone->soa_nx_rrset->rrs = region_alloc(db->region, 
					sizeof(rr_type));
			}
			memcpy(zone->soa_nx_rrset->rrs, rrset->rrs, sizeof(rr_type));
			memcpy(&soa_minimum, rdata_atom_data(rrset->rrs->rdatas[6]),
				rdata_atom_size(rrset->rrs->rdatas[6]));
			if (rrset->rrs->ttl > ntohl(soa_minimum)) {
				rrset->zone->soa_nx_rrset->rrs[0].ttl = ntohl(soa_minimum);
			}
		}
		if(type == TYPE_NS) {
			zone->ns_rrset = rrset;
		}
#ifdef DNSSEC
		if(type == TYPE_RRSIG) {
			int i;
			for (i = 0; i < rrset->rr_count; ++i) {
				if (rr_rrsig_type_covered(&rrset->rrs[i]) == TYPE_SOA) {
					zone->is_secure = 1;
					break;
				}
			}
		}
#endif
	}
}

static zone_type* 
find_zone(namedb_type* db, const dname_type* zone_name, nsd_options_t* opt)
{
	domain_type *domain;
	zone_type* zone;
	domain = domain_table_find(db->domains, zone_name);
	if(!domain) {
		log_msg(LOG_INFO, "xfr: creating domain %s",
			dname_to_string(zone_name,0));
		/* create the zone and domain of apex (zone has config options) */
		domain = domain_table_insert(db->domains, zone_name);
	} else {
		/* O(1) if SOA exists */
		zone = domain_find_zone(domain);
		/* if domain was empty (no rrsets, empty zone) search in zonelist */
		/* check apex to make sure we don't find a parent zone */
		if(!zone || zone->apex != domain)
			zone = namedb_find_zone(db, domain);
		if(zone) {
			assert(zone->apex == domain);
			return zone;
		}
	}
	/* create the zone */
	log_msg(LOG_INFO, "xfr: creating zone_type %s",
		dname_to_string(zone_name,0));
	zone = (zone_type *) region_alloc(db->region, sizeof(zone_type));
	zone->next = db->zones;
	db->zones = zone;
	db->zone_count++;
	zone->apex = domain;
	zone->soa_rrset = 0;
	zone->soa_nx_rrset = 0;
	zone->ns_rrset = 0;
#ifdef NSEC3
	zone->nsec3_rrset = NULL;
	zone->nsec3_last = NULL;
#endif
	zone->opts = zone_options_find(opt, domain_dname(zone->apex)); 
	if(!zone->opts) {
		log_msg(LOG_ERR, "xfr: zone %s not in config.",
			dname_to_string(zone_name,0));
		return 0;
	}
	zone->number = db->zone_count;
	zone->is_secure = 0;
	zone->updated = 1;
	return zone;
}

static void 
delete_zone_rrs(zone_type* zone)
{
	rrset_type *rrset;
	domain_type *domain = zone->apex;
	/* go through entire tree below the zone apex (incl subzones) */
	while(domain && dname_is_subdomain(
		domain_dname(domain), domain_dname(zone->apex)))
	{
		log_msg(LOG_INFO, "delete zone visit %s",
			dname_to_string(domain_dname(domain),0));
		/* delete all rrsets of the zone */
		while((rrset = domain_find_any_rrset(domain, zone))) {
			rrset_delete(domain, rrset);
		}
		domain = domain_next(domain);
	}

	assert(zone->soa_rrset == 0);
	/* keep zone->soa_nx_rrset alloced */
	assert(zone->ns_rrset == 0);
	assert(zone->is_secure == 0);
	assert(zone->updated == 1);
}

static int 
apply_ixfr(namedb_type* db, FILE *in, const off_t* startpos, 
	const char* zone, uint32_t serialno, nsd_options_t* opt,
	uint16_t id, uint32_t seq_nr, uint32_t seq_total,
	int* is_axfr, int* delete_mode, int* rr_count)
{
	uint32_t filelen, msglen;
	int qcount, ancount, counter;
	buffer_type* packet;
	region_type* region;
	int i;
	uint16_t rrlen;
	const dname_type *dname_zone, *dname;
	zone_type* zone_db;
	char file_zone_name[2560];
	uint32_t file_serial, file_seq_nr;
	uint16_t file_id;

	if(fseeko(in, *startpos, SEEK_SET) == -1) {
		log_msg(LOG_INFO, "could not fseeko: %s.", strerror(errno));
		return 0;
	}
	/* read ixfr packet RRs and apply to in memory db */

	if(!read_32(in, &filelen)) {
		log_msg(LOG_ERR, "could not read len");
		return 0;
	}

	/* read header */
	if(filelen < QHEADERSZ + sizeof(uint32_t)*3 + sizeof(uint16_t)) {
		log_msg(LOG_ERR, "msg too short");
		return 0;
	}

	region = region_create(xalloc, free);
	if(!region) {
		log_msg(LOG_ERR, "out of memory");
		return 0;
	}

	if(!read_str(in, file_zone_name, sizeof(file_zone_name)) ||
		!read_32(in, &file_serial) ||
		!read_16(in, &file_id) ||
		!read_32(in, &file_seq_nr))
	{
		log_msg(LOG_ERR, "could not part data");
		return 0;
	}
	
	if(strcmp(file_zone_name, zone) != 0 || serialno != file_serial ||
		id != file_id || seq_nr != file_seq_nr) {
		log_msg(LOG_ERR, "internal error: reading part with changed id");
		return 0;
	}
	msglen = filelen - sizeof(uint32_t)*3 - sizeof(uint16_t)
		- strlen(file_zone_name);
	packet = buffer_create(region, QIOBUFSZ);
	dname_zone = dname_parse(region, zone);
	zone_db = find_zone(db, dname_zone, opt);
	if(!zone_db) {
		log_msg(LOG_ERR, "no zone exists");
		region_destroy(region);
		return 0;
	}
	
	if(msglen > QIOBUFSZ) {
		log_msg(LOG_ERR, "msg too long");
		region_destroy(region);
		return 0;
	}
	buffer_clear(packet);
	if(fread(buffer_begin(packet), msglen, 1, in) != 1) {
		log_msg(LOG_ERR, "short fread: %s", strerror(errno));
		region_destroy(region);
		return 0;
	}
	buffer_set_limit(packet, msglen);

	/* only answer section is really used, question, additional and 
	   authority section RRs are skipped */
	qcount = QDCOUNT(packet);
	ancount = ANCOUNT(packet);
	buffer_skip(packet, QHEADERSZ);

	/* skip queries */
	for(i=0; i<qcount; ++i)
		if(!packet_skip_rr(packet, 1)) {
			log_msg(LOG_ERR, "bad RR in question section");
			region_destroy(region);
			return 0;
		}

	/* first RR: check if SOA and correct zone & serialno */
	if(*rr_count == 0) {
		dname = dname_make_from_packet(region, packet, 1, 1);
		if(!dname) {
			log_msg(LOG_ERR, "could not parse dname");
			region_destroy(region);
			return 0;
		}
		if(dname_compare(dname_zone, dname) != 0) {
			log_msg(LOG_ERR, "SOA dname %s not equal to zone",
				dname_to_string(dname,0));
			log_msg(LOG_ERR, "zone dname is %s",
				dname_to_string(dname_zone,0));
			region_destroy(region);
			return 0;
		}
		if(!buffer_available(packet, 10)) {
			log_msg(LOG_ERR, "bad SOA RR");
			region_destroy(region);
			return 0;
		}
		if(buffer_read_u16(packet) != TYPE_SOA ||
			buffer_read_u16(packet) != CLASS_IN) {
			log_msg(LOG_ERR, "first RR not SOA IN");
			region_destroy(region);
			return 0;
		}
		buffer_skip(packet, sizeof(uint32_t)); /* ttl */
		if(!buffer_available(packet, buffer_read_u16(packet)) ||
			!packet_skip_dname(packet) /* skip prim_ns */ ||
			!packet_skip_dname(packet) /* skip email */) {
			log_msg(LOG_ERR, "bad SOA RR");
			region_destroy(region);
			return 0;
		}
		if(buffer_read_u32(packet) != serialno) {
			buffer_skip(packet, -4);
			log_msg(LOG_ERR, "SOA serial %d different from commit %d",
				buffer_read_u32(packet), serialno);
			region_destroy(region);
			return 0;
		}
		buffer_skip(packet, sizeof(uint32_t)*4);
		counter = 1;
		*rr_count = 1;
		*is_axfr = 0;
		*delete_mode = 0;
	}
	else  counter = 0;

	for(; counter < ancount; ++counter,++(*rr_count))
	{
		uint16_t type, klass;
		uint32_t ttl;

		if(!(dname=dname_make_from_packet(region, packet, 1,1))) {
			log_msg(LOG_ERR, "bad xfr RR dname %d", *rr_count);
			region_destroy(region);
			return 0;
		}
		if(!buffer_available(packet, 10)) {
			log_msg(LOG_ERR, "bad xfr RR format %d", *rr_count);
			region_destroy(region);
			return 0;
		}
		type = buffer_read_u16(packet);
		klass = buffer_read_u16(packet);
		ttl = buffer_read_u32(packet);
		rrlen = buffer_read_u16(packet);
		if(!buffer_available(packet, rrlen)) {
			log_msg(LOG_ERR, "bad xfr RR rdata %d, len %d have %zd", 
				*rr_count, rrlen, buffer_remaining(packet));
			region_destroy(region);
			return 0;
		}

		if(*rr_count == 1 && type != TYPE_SOA) {
			/* second RR: if not SOA: this is an AXFR; delete all zone contents */
			delete_zone_rrs(zone_db);
			/* add everything else (incl end SOA) */
			*delete_mode = 0;
			*is_axfr = 1;
		}
		if(type == TYPE_SOA && !*is_axfr) {
			/* switch from delete-part to add-part and back again,
			   just before soa - so it gets deleted and added too */
			/* this means we switch to delete mode for the final SOA */
			*delete_mode = !*delete_mode;
		}
		if(type == TYPE_TSIG || type == TYPE_OPT) {
			/* ignore pseudo RRs */
			continue;
		}
		log_msg(LOG_INFO, "xfr %s RR dname is %s type %s", 
			*delete_mode?"del":"add",
			dname_to_string(dname,0), rrtype_to_string(type));
		if(*delete_mode) {
			/* delete this rr */
			if(!is_axfr && type == TYPE_SOA && counter==ancount-1
				&& seq_nr == seq_total-1)
				continue; /* do not delete final SOA RR for IXFR */
			delete_RR(db, dname, type, klass, ttl, packet, rrlen, zone_db,
				region);
		}
		else
		{
			/* add this rr */
			add_RR(db, dname, type, klass, ttl, packet, rrlen, zone_db);
		}
	}
	region_destroy(region);
	return 1;
}

static int 
check_for_bad_serial(namedb_type* db, const char* zone_str, uint32_t old_serial)
{
	/* see if serial OK with in-memory serial */
	domain_type* domain;
	region_type* region = region_create(xalloc, free);
	const dname_type* zone_name = dname_parse(region, zone_str);
	zone_type* zone = 0;
	domain = domain_table_find(db->domains, zone_name);
	if(domain)
		zone = domain_find_zone(domain);
	if(zone && zone->apex == domain && zone->soa_rrset && old_serial)
	{
		uint32_t memserial;
		memcpy(&memserial, rdata_atom_data(zone->soa_rrset->rrs[0].rdatas[2]), 
			sizeof(uint32_t));
		if(old_serial != ntohl(memserial)) {
			region_destroy(region);
			return 1;
		}
	}
	region_destroy(region);
	return 0;
}

/* for multiple tcp packets use a data structure that has
 * a rbtree (zone_names) with for each zone:
 * 	has a rbtree by sequence number
 *		with inside a serial_number and ID (for checking only)
 *		and contains a off_t to the IXFR packet in the file.
 * so when you get a commit for a zone, get zone obj, find sequence,
 * then check if you have all sequence numbers available. Apply all packets.
 */
struct diff_read_data {
	/* rbtree of struct diff_zone*/
	rbtree_t* zones;
	/* region for allocation */
	region_type* region;
};
struct diff_zone {
	/* key is dname of zone */
	rbnode_t node;
	/* rbtree of struct diff_xfrpart */
	rbtree_t* parts;
};
struct diff_xfrpart {
	/* key is sequence number */
	rbnode_t node;
	uint32_t seq_nr;
	uint32_t new_serial;
	uint16_t id;
	off_t file_pos;
};

static struct diff_read_data* 
diff_read_data_create()
{
	region_type* region = region_create(xalloc, free);
	struct diff_read_data* data = (struct diff_read_data*)
		region_alloc(region, sizeof(struct diff_read_data));
	data->region = region;
	data->zones = rbtree_create(region, 
		(int (*)(const void *, const void *)) dname_compare);
	return data;
}

static struct diff_zone*
diff_read_find_zone(struct diff_read_data* data, const char* name)
{
	const dname_type* dname = dname_parse(data->region, name);
	struct diff_zone* zp = (struct diff_zone*)
		rbtree_search(data->zones, dname);
	return zp;
}

static int intcompf(const void* a, const void* b)
{
	if(*(uint32_t*)a < *(uint32_t*)b) 
		return -1;
	if(*(uint32_t*)a > *(uint32_t*)b) 
		return +1;
	return 0;
}

static struct diff_zone*
diff_read_insert_zone(struct diff_read_data* data, const char* name)
{
	const dname_type* dname = dname_parse(data->region, name);
	struct diff_zone* zp = region_alloc(data->region,
		sizeof(struct diff_zone));
	zp->node = *RBTREE_NULL;
	zp->node.key = dname;
	zp->parts = rbtree_create(data->region, intcompf);
	rbtree_insert(data->zones, (rbnode_t*)zp);
	return zp;
}

static struct diff_xfrpart*
diff_read_find_part(struct diff_zone* zp, uint32_t seq_nr)
{
	struct diff_xfrpart* xp = (struct diff_xfrpart*)
		rbtree_search(zp->parts, &seq_nr);
	return xp;
}

static struct diff_xfrpart*
diff_read_insert_part(struct diff_read_data* data,
	struct diff_zone* zp, uint32_t seq_nr)
{
	struct diff_xfrpart* xp = region_alloc(data->region,
		sizeof(struct diff_xfrpart));
	xp->node = *RBTREE_NULL;
	xp->node.key = &xp->seq_nr;
	xp->seq_nr = seq_nr;
	rbtree_insert(zp->parts, (rbnode_t*)xp);
	return xp;
}

static int 
read_sure_part(namedb_type* db, FILE *in, nsd_options_t* opt, 
	struct diff_read_data* data, struct diff_log** log)
{
	char zone_buf[512];
	char log_buf[5120];
	uint32_t old_serial, new_serial, num_parts;
	uint16_t id;
	uint8_t committed;
	struct diff_zone *zp;
	uint32_t i;
	int have_all_parts = 1;
	struct diff_log* thislog = 0;

	/* read zone name and serial */
	if(!read_str(in, zone_buf, sizeof(zone_buf)) ||
		!read_32(in, &old_serial) ||
		!read_32(in, &new_serial) ||
		!read_16(in, &id) ||
		!read_32(in, &num_parts) ||
		!read_8(in, &committed) ||
		!read_str(in, log_buf, sizeof(log_buf)) )
	{
		log_msg(LOG_ERR, "diff file bad commit part");
		return 1;
	}

	if(log) {
		thislog = (struct diff_log*)region_alloc(db->region, sizeof(struct diff_log));
		thislog->zone_name = region_strdup(db->region, zone_buf);
		thislog->comment = region_strdup(db->region, log_buf);
		thislog->error = 0;
		thislog->next = *log;
		*log = thislog;
	}

	/* has been read in completely */
	zp = diff_read_find_zone(data, zone_buf);
	if(!zp) {
		log_msg(LOG_ERR, "diff file commit without IXFR");
		if(thislog)
			thislog->error = "error no IXFR parts";
		return 1;
	}
	if(committed && check_for_bad_serial(db, zone_buf, old_serial)) {
		log_msg(LOG_ERR, "diff file commit with bad serial");
		zp->parts->root = RBTREE_NULL;
		zp->parts->count = 0;
		if(thislog)
			thislog->error = "error bad serial";
		return 1;
	}
	for(i=0; i<num_parts; i++) {
		struct diff_xfrpart *xp = diff_read_find_part(zp, i);
		if(!xp || xp->id != id || xp->new_serial != new_serial) {
			have_all_parts = 0;
		}
	}
	if(!have_all_parts) {
		log_msg(LOG_ERR, "diff file commit without all parts");
		if(thislog)
			thislog->error = "error missing parts";
	}

	if(committed && have_all_parts)
	{
		int is_axfr=0, delete_mode=0, rr_count=0;
		off_t resume_pos;

		log_msg(LOG_INFO, "processing xfr: %s", log_buf);
		resume_pos = ftello(in);
		if(resume_pos == -1) {
			log_msg(LOG_INFO, "could not ftello: %s.", strerror(errno));
			return 0;
		}
		for(i=0; i<num_parts; i++) {
			struct diff_xfrpart *xp = diff_read_find_part(zp, i);
			log_msg(LOG_INFO, "processing xfr: apply part %d", (int)i);
			if(!apply_ixfr(db, in, &xp->file_pos, zone_buf, new_serial, opt,
				id, xp->seq_nr, num_parts, &is_axfr, &delete_mode,
				&rr_count)) {
				log_msg(LOG_ERR, "bad ixfr packet part %d", (int)i);
			}
		}
		if(fseeko(in, resume_pos, SEEK_SET) == -1) {
			log_msg(LOG_INFO, "could not fseeko: %s.", strerror(errno));
			return 0;
		}
	}
	else 	log_msg(LOG_INFO, "skipping xfr: %s", log_buf);
	
	/* clean out the parts for the zone after the commit/rollback */
	zp->parts->root = RBTREE_NULL;
	zp->parts->count = 0;
	return 1;
}

static int
store_ixfr_data(FILE *in, uint32_t len, struct diff_read_data* data, off_t* startpos)
{
	char zone_name[2560];
	struct diff_zone* zp;
	struct diff_xfrpart* xp;
	uint32_t new_serial, seq;
	uint16_t id;
	if(!read_str(in, zone_name, sizeof(zone_name)) ||
		!read_32(in, &new_serial) ||
		!read_16(in, &id) ||
		!read_32(in, &seq)) {
		log_msg(LOG_INFO, "could not read ixfr store info: %s", strerror(errno));
		return 0;
	}
	len -= sizeof(uint32_t)*3 + sizeof(uint16_t) + strlen(zone_name);
	if(fseeko(in, len, SEEK_CUR) == -1)
		log_msg(LOG_INFO, "fseek failed: %s", strerror(errno));
	/* store the info */
	zp = diff_read_find_zone(data, zone_name);
	if(!zp)
		zp = diff_read_insert_zone(data, zone_name);
	xp = diff_read_find_part(zp, seq);
	if(xp) {
		log_msg(LOG_INFO, "duplicate xfr part: %s %d", zone_name, seq);
		/* overwrite with newer value (which probably relates to next commit) */
	}
	else {
		xp = diff_read_insert_part(data, zp, seq);
	}
	xp->new_serial = new_serial;
	xp->id = id;
	xp->file_pos = *startpos;
	return 1;
}

static int 
read_process_part(namedb_type* db, FILE *in, uint32_t type,
	nsd_options_t* opt, struct diff_read_data* data,
	struct diff_log** log)
{
	uint32_t len, len2;
	off_t startpos;

	startpos = ftello(in);
	if(startpos == -1) {
		log_msg(LOG_INFO, "could not ftello: %s.", strerror(errno));
		return 0;
	}

	if(!read_32(in, &len)) return 1;

	if(type == DIFF_PART_IXFR) {
		log_msg(LOG_INFO, "part IXFR len %d", len);
		if(!store_ixfr_data(in, len, data, &startpos))
			return 0;
	}
	else if(type == DIFF_PART_SURE) {
		log_msg(LOG_INFO, "part SURE len %d", len);
		if(!read_sure_part(db, in, opt, data, log)) 
			return 0;
	} else {
		log_msg(LOG_INFO, "unknown part %x len %d", type, len);
		return 0;
	}
	if(!read_32(in, &len2))
		return 1; /* short read is OK */
	if(len != len2)
		return 0; /* bad data is wrong */
	return 1;
}

/*
 * Finds smallest offset in data structs
 * returns 0 if no offsets in the data structs.
 */
static int
find_smallest_offset(struct diff_read_data* data, off_t* offset)
{
	int found_any = 0;
	struct diff_zone* dz;
	struct diff_xfrpart* dx;
	if(!data || !data->zones)
		return 0;
	RBTREE_FOR(dz, struct diff_zone*, data->zones)
	{
		if(!dz->parts)
			continue;
		RBTREE_FOR(dx, struct diff_xfrpart*, dz->parts)
		{
			if(found_any) {
				if(dx->file_pos < *offset)
					*offset = dx->file_pos;
			} else {
				found_any = 1;
				*offset = dx->file_pos;
			}
		}
	}

	return found_any;
}

int 
diff_read_file(namedb_type* db, nsd_options_t* opt, struct diff_log** log)
{
	const char* filename = DIFFFILE;
	FILE *df;
	uint32_t type;
	struct diff_read_data* data = diff_read_data_create();

	if(opt->difffile) 
		filename = opt->difffile;

	df = fopen(filename, "r");
	if(!df) {
		log_msg(LOG_INFO, "could not open file %s for reading: %s",
			filename, strerror(errno));
		region_destroy(data->region);
		return 1;
	}
	if(db->diff_skip) {
		log_msg(LOG_INFO, "skip diff file");
		if(fseeko(df, db->diff_pos, SEEK_SET)==-1) {
			log_msg(LOG_INFO, "could not fseeko file %s: %s. Reread from start.",
				filename, strerror(errno));
		}
	}

	while(read_32(df, &type)) 
	{
		log_msg(LOG_INFO, "iter loop");
		if(!read_process_part(db, df, type, opt, data, log))
		{
			log_msg(LOG_INFO, "error processing diff file");
			region_destroy(data->region);
			return 0;
		}
	}
	log_msg(LOG_INFO, "end of diff file read");

	if(find_smallest_offset(data, &db->diff_pos)) {
		/* can skip to the first unused element */
		db->diff_skip = 1;
	} else {
		/* all processed, can skip to here next time */
		db->diff_skip = 1;
		db->diff_pos = ftello(df);
		if(db->diff_pos == -1) {
			log_msg(LOG_INFO, "could not ftello: %s.",
				strerror(errno));
			db->diff_skip = 0;
		}
	}
	
	region_destroy(data->region);
	fclose(df);
	return 1;
}

static int diff_broken(FILE *df, off_t* break_pos)
{
	uint32_t type, len, len2;
	*break_pos = ftello(df);

	/* try to read and validate parts of the file */
	while(read_32(df, &type)) /* cannot read type is no error, normal EOF */
	{
		/* check type */
		if(type != DIFF_PART_IXFR && type != DIFF_PART_SURE)
			return 1;
		/* check length */
		if(!read_32(df, &len))
			return 1; /* EOF inside the part is error */
		if(fseeko(df, len, SEEK_CUR) == -1)
		{
			log_msg(LOG_INFO, "fseeko failed: %s", strerror(errno));
			return 1;
		}
		/* fseek clears EOF flag, but try reading length value,
		   if EOF, the part is truncated */
		if(!read_32(df, &len2))
			return 1;
		if(len != len2)
			return 1; /* bad part, lengths must agree */
		/* this part is ok */
		*break_pos = ftello(df);
	}
	return 0;
}

void diff_snip_garbage(namedb_type* db, nsd_options_t* opt)
{
	off_t break_pos;
	const char* filename = DIFFFILE;
	FILE *df;

	/* open file here and keep open, so it cannot change under our nose */
	if(opt->difffile)
		filename = opt->difffile;
	df = fopen(filename, "r+");
	if(!df) {
		log_msg(LOG_INFO, "could not open file %s for garbage collecting: %s",
			filename, strerror(errno));
		return;
	}
	/* and skip into file, since nsd does not read anything before the pos */
	if(db->diff_skip) {
		log_msg(LOG_INFO, "garbage collect skip diff file");
		if(fseeko(df, db->diff_pos, SEEK_SET)==-1) {
			log_msg(LOG_INFO, "could not fseeko file %s: %s.", 
				filename, strerror(errno));
			fclose(df);
			return;
		}
	}

	/* detect break point */
	if(diff_broken(df, &break_pos))
	{
		/* snip off at break_pos */
		log_msg(LOG_INFO, "snipping off trailing partial part of %s", 
			filename);
		ftruncate(fileno(df), break_pos);
	}

	fclose(df);
}

// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MDS.h"
#include "MDCache.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"
#include "Migrator.h"
#include "Locker.h"
#include "Migrator.h"

#include "MDBalancer.h"
#include "MDLog.h"
#include "MDSMap.h"

#include "include/filepath.h"

#include "events/EString.h"
#include "events/EExport.h"
#include "events/EImportStart.h"
#include "events/EImportFinish.h"

#include "msg/Messenger.h"

#include "messages/MClientFileCaps.h"

#include "messages/MExportDirDiscover.h"
#include "messages/MExportDirDiscoverAck.h"
#include "messages/MExportDirPrep.h"
#include "messages/MExportDirPrepAck.h"
#include "messages/MExportDirWarning.h"
#include "messages/MExportDirWarningAck.h"
#include "messages/MExportDir.h"
#include "messages/MExportDirAck.h"
#include "messages/MExportDirNotify.h"
#include "messages/MExportDirNotifyAck.h"
#include "messages/MExportDirFinish.h"

#include "messages/MHashDirDiscover.h"
#include "messages/MHashDirDiscoverAck.h"
#include "messages/MHashDirPrep.h"
#include "messages/MHashDirPrepAck.h"
#include "messages/MHashDir.h"
#include "messages/MHashDirNotify.h"
#include "messages/MHashDirAck.h"

#include "messages/MUnhashDirPrep.h"
#include "messages/MUnhashDirPrepAck.h"
#include "messages/MUnhashDir.h"
#include "messages/MUnhashDirAck.h"
#include "messages/MUnhashDirNotify.h"
#include "messages/MUnhashDirNotifyAck.h"


#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) cout << g_clock.now() << " mds" << mds->get_nodeid() << ".migrator "



void Migrator::dispatch(Message *m)
{
  switch (m->get_type()) {
    // import
  case MSG_MDS_EXPORTDIRDISCOVER:
    handle_export_discover((MExportDirDiscover*)m);
    break;
  case MSG_MDS_EXPORTDIRPREP:
    handle_export_prep((MExportDirPrep*)m);
    break;
  case MSG_MDS_EXPORTDIR:
    handle_export_dir((MExportDir*)m);
    break;
  case MSG_MDS_EXPORTDIRFINISH:
    handle_export_finish((MExportDirFinish*)m);
    break;

    // export 
  case MSG_MDS_EXPORTDIRDISCOVERACK:
    handle_export_discover_ack((MExportDirDiscoverAck*)m);
    break;
  case MSG_MDS_EXPORTDIRPREPACK:
    handle_export_prep_ack((MExportDirPrepAck*)m);
    break;
  case MSG_MDS_EXPORTDIRWARNINGACK:
    handle_export_warning_ack((MExportDirWarningAck*)m);
    break;
  case MSG_MDS_EXPORTDIRACK:
    handle_export_ack((MExportDirAck*)m);
    break;
  case MSG_MDS_EXPORTDIRNOTIFYACK:
    handle_export_notify_ack((MExportDirNotifyAck*)m);
    break;    

    // export 3rd party (dir_auth adjustments)
  case MSG_MDS_EXPORTDIRWARNING:
    handle_export_warning((MExportDirWarning*)m);
    break;
  case MSG_MDS_EXPORTDIRNOTIFY:
    handle_export_notify((MExportDirNotify*)m);
    break;


    // hashing
    /*
  case MSG_MDS_HASHDIRDISCOVER:
    handle_hash_dir_discover((MHashDirDiscover*)m);
    break;
  case MSG_MDS_HASHDIRDISCOVERACK:
    handle_hash_dir_discover_ack((MHashDirDiscoverAck*)m);
    break;
  case MSG_MDS_HASHDIRPREP:
    handle_hash_dir_prep((MHashDirPrep*)m);
    break;
  case MSG_MDS_HASHDIRPREPACK:
    handle_hash_dir_prep_ack((MHashDirPrepAck*)m);
    break;
  case MSG_MDS_HASHDIR:
    handle_hash_dir((MHashDir*)m);
    break;
  case MSG_MDS_HASHDIRACK:
    handle_hash_dir_ack((MHashDirAck*)m);
    break;
  case MSG_MDS_HASHDIRNOTIFY:
    handle_hash_dir_notify((MHashDirNotify*)m);
    break;

    // unhashing
  case MSG_MDS_UNHASHDIRPREP:
    handle_unhash_dir_prep((MUnhashDirPrep*)m);
    break;
  case MSG_MDS_UNHASHDIRPREPACK:
    handle_unhash_dir_prep_ack((MUnhashDirPrepAck*)m);
    break;
  case MSG_MDS_UNHASHDIR:
    handle_unhash_dir((MUnhashDir*)m);
    break;
  case MSG_MDS_UNHASHDIRACK:
    handle_unhash_dir_ack((MUnhashDirAck*)m);
    break;
  case MSG_MDS_UNHASHDIRNOTIFY:
    handle_unhash_dir_notify((MUnhashDirNotify*)m);
    break;
  case MSG_MDS_UNHASHDIRNOTIFYACK:
    handle_unhash_dir_notify_ack((MUnhashDirNotifyAck*)m);
    break;
    */

  default:
    assert(0);
  }
}


class C_MDC_EmptyImport : public Context {
  Migrator *mig;
  CDir *dir;
public:
  C_MDC_EmptyImport(Migrator *m, CDir *d) : mig(m), dir(d) {}
  void finish(int r) {
    mig->export_empty_import(dir);
  }
};


void Migrator::export_empty_import(CDir *dir)
{
  dout(7) << "export_empty_import " << *dir << endl;
  
  if (dir->inode->is_auth()) return;
  if (!dir->is_auth()) return;
  
  if (dir->inode->is_freezing() || dir->inode->is_frozen()) return;
  if (dir->is_freezing() || dir->is_frozen()) return;
  
  if (dir->get_size() > 0) {
    dout(7) << "not actually empty" << endl;
    return;
  }

  if (dir->inode->is_root()) {
    dout(7) << "root" << endl;
    return;
  }
  
  // is it really empty?
  if (!dir->is_complete()) {
    dout(7) << "not complete, fetching." << endl;
    dir->fetch(new C_MDC_EmptyImport(this,dir));
    return;
  }
  
  int dest = dir->inode->authority().first;
  
  // comment this out ot wreak havoc?
  //if (mds->is_shutting_down()) dest = 0;  // this is more efficient.
  
  dout(7) << "really empty, exporting to " << dest << endl;
  assert (dest != mds->get_nodeid());
  
  dout(-7) << "exporting to mds" << dest 
           << " empty import " << *dir << endl;
  export_dir( dir, dest );
}




// ==========================================================
// mds failure handling

void Migrator::handle_mds_failure(int who)
{
  dout(5) << "handle_mds_failure mds" << who << endl;

  // check my exports
  map<CDir*,int>::iterator p = export_state.begin();
  while (p != export_state.end()) {
    map<CDir*,int>::iterator next = p;
    next++;
    CDir *dir = p->first;
    
    if (export_peer[dir] == who) {
      // the guy i'm exporting to failed.  
      // clean up.
      dout(10) << "cleaning up export state " << p->second << " of " << *dir << endl;
      
      switch (p->second) {
      case EXPORT_DISCOVERING:
	dout(10) << "export state=discovering : canceling freeze and removing auth_pin" << endl;
	dir->unfreeze_tree();  // cancel the freeze
	dir->auth_unpin();     // remove the auth_pin (that was holding up the freeze)
	export_state.erase(dir); // clean up
	break;

      case EXPORT_FREEZING:
	dout(10) << "export state=freezing : canceling freeze" << endl;
	dir->unfreeze_tree();  // cancel the freeze
	export_state.erase(dir); // clean up
	break;

	// NOTE: state order reversal, warning comes after loggingstart+prepping
      case EXPORT_WARNING:
	dout(10) << "export state=warning : unpinning bounds, unfreezing, notifying" << endl;
	//export_notify_abort(dir);   // tell peers about abort

	// fall-thru

	//case EXPORT_LOGGINGSTART:
      case EXPORT_PREPPING:
	if (p->second != EXPORT_WARNING) 
	  dout(10) << "export state=loggingstart|prepping : unpinning bounds, unfreezing" << endl;
	// unpin bounds
	for (set<CDir*>::iterator p = export_bounds[dir].begin();
	     p != export_bounds[dir].end();
	     ++p) {
	  CDir *bd = *p;
	  bd->put(CDir::PIN_EXPORTBOUND);
	  bd->state_clear(CDir::STATE_EXPORTBOUND);
	}
	dir->unfreeze_tree();
	cache->adjust_subtree_auth(dir, mds->get_nodeid());
	cache->try_subtree_merge(dir);
	export_state.erase(dir); // clean up
	break;
	
      case EXPORT_EXPORTING:
	dout(10) << "export state=exporting : reversing, and unfreezing" << endl;
	export_reverse(dir);
	export_state.erase(dir); // clean up
	break;

      case EXPORT_LOGGINGFINISH:
      case EXPORT_NOTIFYING:
	dout(10) << "export state=loggingfinish|notifying : ignoring dest failure, we were successful." << endl;
	// leave export_state, don't clean up now.
	break;

      default:
	assert(0);
      }

      // finish clean-up?
      if (export_state.count(dir) == 0) {
	export_peer.erase(dir);
	export_warning_ack_waiting.erase(dir);
	export_notify_ack_waiting.erase(dir);
	
	// unpin the path
	vector<CDentry*> trace;
	cache->make_trace(trace, dir->inode);
	cache->path_unpin(trace, 0);
	
	// wake up any waiters
	mds->queue_finished(export_finish_waiters[dir]);
	export_finish_waiters.erase(dir);
	
	// send pending import_maps?  (these need to go out when all exports have finished.)
	cache->send_pending_import_maps();
	
	cache->show_subtrees();
      }
    } else {
      // bystander failed.
      if (p->second == EXPORT_WARNING) {
	// exporter waiting for warning acks, let's fake theirs.
	if (export_warning_ack_waiting[dir].count(who)) {
	  dout(10) << "faking export_warning_ack from mds" << who
		   << " on " << *dir << " to mds" << export_peer[dir] 
		   << endl;
	  export_warning_ack_waiting[dir].erase(who);
	  export_notify_ack_waiting[dir].erase(who);   // they won't get a notify either.
	  if (export_warning_ack_waiting[dir].empty()) 
	    export_go(dir);
	}
      }
      if (p->second == EXPORT_NOTIFYING) {
	// exporter is waiting for notify acks, fake it
	if (export_notify_ack_waiting[dir].count(who)) {
	  dout(10) << "faking export_notify_ack from mds" << who
		   << " on " << *dir << " to mds" << export_peer[dir] 
		   << endl;
	  export_notify_ack_waiting[dir].erase(who);
	  if (export_notify_ack_waiting[dir].empty()) 
	    export_finish(dir);
	}
      }
    }
    
    // next!
    p = next;
  }


  // check my imports
  map<inodeno_t,int>::iterator q = import_state.begin();
  while (q != import_state.end()) {
    map<inodeno_t,int>::iterator next = q;
    next++;
    inodeno_t dirino = q->first;
    CInode *diri = mds->mdcache->get_inode(dirino);
    CDir *dir = mds->mdcache->get_dir(dirino);

    if (import_peer[dirino] == who) {
      switch (import_state[dirino]) {
      case IMPORT_DISCOVERED:
	dout(10) << "import state=discovered : unpinning inode " << *diri << endl;
	assert(diri);
	// unpin base
	diri->put(CInode::PIN_IMPORTING);
	import_state.erase(dirino);
	import_peer.erase(dirino);
	break;

      case IMPORT_PREPPING:
	if (import_state[dirino] == IMPORT_PREPPING) {
	  dout(10) << "import state=prepping : unpinning base+bounds " << *dir << endl;
	}
	assert(dir);
	import_reverse_unpin(dir);    // unpin
	break;

      case IMPORT_PREPPED:
	dout(10) << "import state=prepping : unpinning base+bounds, unfreezing " << *dir << endl;
	assert(dir);
	
	// adjust auth back to me
	cache->adjust_subtree_auth(dir, import_peer[dirino]);
	cache->try_subtree_merge(dir);
	
	// bystanders?
	if (import_bystanders[dir].empty()) {
	  import_reverse_unfreeze(dir);
	} else {
	  // notify them; wait in aborting state
	  import_notify_abort(dir);
	  import_state[dirino] = IMPORT_ABORTING;
	}
	break;

      case IMPORT_LOGGINGSTART:
	dout(10) << "import state=loggingstart : reversing import on " << *dir << endl;
	import_reverse(dir);
	break;

      case IMPORT_ACKING:
	// hrm.  make this an ambiguous import, and wait for exporter recovery to disambiguate
	dout(10) << "import state=acking : noting ambiguous import " << *dir << endl;
	cache->add_ambiguous_import(dir, import_bounds[dir]);
	break;

      case IMPORT_ABORTING:
	dout(10) << "import state=aborting : ignoring repeat failure " << *dir << endl;
	break;
      }
    }

    // next!
    q = next;
  }
}






// ==========================================================
// EXPORT


class C_MDC_ExportFreeze : public Context {
  Migrator *mig;
  CDir *ex;   // dir i'm exporting
  int dest;

public:
  C_MDC_ExportFreeze(Migrator *m, CDir *e, int d) :
	mig(m), ex(e), dest(d) {}
  virtual void finish(int r) {
    if (r >= 0)
      mig->export_frozen(ex, dest);
  }
};



/** export_dir(dir, dest)
 * public method to initiate an export.
 * will fail if the directory is freezing, frozen, unpinnable, or root. 
 */
void Migrator::export_dir(CDir *dir,
			  int dest)
{
  dout(7) << "export_dir " << *dir << " to " << dest << endl;
  assert(dir->is_auth());
  assert(dest != mds->get_nodeid());
  assert(!dir->is_hashed());
   
  if (mds->mdsmap->is_degraded()) {
    dout(7) << "cluster degraded, no exports for now" << endl;
    return;
  }

  if (dir->inode->is_root()) {
    dout(7) << "i won't export root" << endl;
    //assert(0);
    return;
  }

  if (dir->is_frozen() ||
      dir->is_freezing()) {
    dout(7) << " can't export, freezing|frozen.  wait for other exports to finish first." << endl;
    return;
  }
  if (dir->is_hashed()) {
    dout(7) << "can't export hashed dir right now.  implement me carefully later." << endl;
    return;
  }
  

  // pin path?
  vector<CDentry*> trace;
  cache->make_trace(trace, dir->inode);
  if (!cache->path_pin(trace, 0, 0)) {
    dout(7) << "export_dir couldn't pin path, failing." << endl;
    return;
  }

  // ok, let's go.
  assert(export_state.count(dir) == 0);
  export_state[dir] = EXPORT_DISCOVERING;
  export_peer[dir] = dest;

  // send ExportDirDiscover (ask target)
  mds->send_message_mds(new MExportDirDiscover(dir->inode), dest, MDS_PORT_MIGRATOR);
  dir->auth_pin();   // pin dir, to hang up our freeze  (unpin on prep ack)

  // take away the popularity we're sending.   FIXME: do this later?
  mds->balancer->subtract_export(dir);
  
  // freeze the subtree
  dir->freeze_tree(new C_MDC_ExportFreeze(this, dir, dest));
}


/*
 * called on receipt of MExportDirDiscoverAck
 * the importer now has the directory's _inode_ in memory, and pinned.
 */
void Migrator::handle_export_discover_ack(MExportDirDiscoverAck *m)
{
  CInode *in = cache->get_inode(m->get_ino());
  assert(in);
  CDir *dir = in->dir;
  assert(dir);
  
  dout(7) << "export_discover_ack from " << m->get_source()
	  << " on " << *dir << ", releasing auth_pin" << endl;

  export_state[dir] = EXPORT_FREEZING;

  dir->auth_unpin();   // unpin to allow freeze to complete
  
  delete m;  // done
}


void Migrator::export_frozen(CDir *dir,
			     int dest)
{
  // subtree is now frozen!
  dout(7) << "export_frozen on " << *dir << " to " << dest << endl;

  if (export_state.count(dir) == 0 ||
      export_state[dir] != EXPORT_FREEZING) {
    dout(7) << "dest must have failed, aborted" << endl;
    return;
  }

  assert(dir->is_frozen());

  // ok!
  //export_state[dir] = EXPORT_LOGGINGSTART;

  cache->show_subtrees();

  // note the bounds.
  //  force it into a subtree by listing auth as <me,me>.
  cache->adjust_subtree_auth(dir, mds->get_nodeid(), mds->get_nodeid());
  cache->get_subtree_bounds(dir, export_bounds[dir]);
  set<CDir*> &bounds = export_bounds[dir];

  // generate prep message, log entry.
  MExportDirPrep *prep = new MExportDirPrep(dir->inode);

  // include list of bystanders
  for (map<int,int>::iterator p = dir->replicas_begin();
       p != dir->replicas_end();
       p++) {
    if (p->first != dest) {
      dout(10) << "bystander mds" << p->first << endl;
      prep->add_bystander(p->first);
    }
  }

  // include spanning tree for all nested exports.
  // these need to be on the destination _before_ the final export so that
  // dir_auth updates on any nested exports are properly absorbed.
  set<inodeno_t> inodes_added;

  // include base dir
  prep->add_dir( new CDirDiscover(dir, dir->add_replica(dest)) );
  
  // check bounds
  for (set<CDir*>::iterator it = bounds.begin();
       it != bounds.end();
       it++) {
    CDir *bound = *it;

    // pin it.
    bound->get(CDir::PIN_EXPORTBOUND);
    bound->state_set(CDir::STATE_EXPORTBOUND);
    
    dout(7) << "  export bound " << *bound << endl;

    prep->add_export( bound->ino() );

    /* first assemble each trace, in trace order, and put in message */
    list<CInode*> inode_trace;  

    // trace to dir
    CDir *cur = bound;
    while (cur != dir) {
      // don't repeat ourselves
      if (inodes_added.count(cur->ino())) break;   // did already!
      inodes_added.insert(cur->ino());
      
      CDir *parent_dir = cur->get_parent_dir();

      // inode?
      assert(cur->inode->is_auth());
      inode_trace.push_front(cur->inode);
      dout(7) << "  will add " << *cur->inode << endl;
      
      // include dir? note: this'll include everything except the nested exports themselves, 
      // since someone else is obviously auth.
      if (cur->is_auth()) {
        prep->add_dir( new CDirDiscover(cur, cur->add_replica(dest)) );  // yay!
        dout(7) << "  added " << *cur << endl;
      }
      
      cur = parent_dir;      
    }

    for (list<CInode*>::iterator it = inode_trace.begin();
         it != inode_trace.end();
         it++) {
      CInode *in = *it;
      dout(7) << "  added " << *in << endl;
      prep->add_inode( in->parent->get_dir()->ino(),
                       in->parent->get_name(),
                       in->replicate_to(dest) );
    }

  }

  // send.
  export_state[dir] = EXPORT_PREPPING;
  mds->send_message_mds(prep, dest, MDS_PORT_MIGRATOR);
}

void Migrator::handle_export_prep_ack(MExportDirPrepAck *m)
{
  CInode *in = cache->get_inode(m->get_ino());
  assert(in);
  CDir *dir = in->dir;
  assert(dir);

  dout(7) << "export_prep_ack " << *dir << endl;

  if (export_state.count(dir) == 0 ||
      export_state[dir] != EXPORT_PREPPING) {
    // export must have aborted.  
    dout(7) << "export must have aborted" << endl;
    delete m;
    return;
  }

  // send warnings
  assert(export_peer.count(dir));
  int dest = export_peer[dir];
  assert(export_warning_ack_waiting.count(dir) == 0);
  assert(export_notify_ack_waiting.count(dir) == 0);
  for (map<int,int>::iterator p = dir->replicas_begin();
       p != dir->replicas_end();
       ++p) {
    if (p->first == dest) continue;
    if (!mds->mdsmap->is_active_or_stopping(p->first))
      continue;  // only if active
    export_warning_ack_waiting[dir].insert(p->first);
    export_notify_ack_waiting[dir].insert(p->first);  // we'll eventually get a notifyack, too!

    //mds->send_message_mds(new MExportDirWarning(dir->ino(), export_peer[dir]),
    //p->first, MDS_PORT_MIGRATOR);
    MExportDirNotify *notify = new MExportDirNotify(dir->ino(), true,
						    pair<int,int>(mds->get_nodeid(),CDIR_AUTH_UNKNOWN),
						    pair<int,int>(mds->get_nodeid(),export_peer[dir]));
    notify->copy_exports(export_bounds[dir]);
    mds->send_message_mds(notify, p->first, MDS_PORT_MIGRATOR);
    
  }
  export_state[dir] = EXPORT_WARNING;

  // nobody to warn?
  if (export_warning_ack_waiting.count(dir) == 0) 
    export_go(dir);  // start export.
    
  // done.
  delete m;
}


void Migrator::handle_export_warning_ack(MExportDirWarningAck *m)
{
  CInode *in = cache->get_inode(m->get_ino());
  assert(in);
  CDir *dir = in->dir;
  assert(dir);
  
  dout(7) << "export_warning_ack " << *dir << " from " << m->get_source() << endl;

  if (export_state.count(dir) == 0 ||
      export_state[dir] != EXPORT_WARNING) {
    // export must have aborted.  
    dout(7) << "export must have aborted" << endl;
    delete m;
    return;
  }

  // process the warning_ack
  int from = m->get_source().num();
  assert(export_warning_ack_waiting.count(dir));
  export_warning_ack_waiting[dir].erase(from);
  
  if (export_warning_ack_waiting[dir].empty()) 
    export_go(dir);     // start export.
    
  // done
  delete m;
}


void Migrator::export_go(CDir *dir)
{  
  assert(export_peer.count(dir));
  int dest = export_peer[dir];
  dout(7) << "export_go " << *dir << " to " << dest << endl;

  cache->show_subtrees();
  
  export_warning_ack_waiting.erase(dir);
  export_state[dir] = EXPORT_EXPORTING;

  assert(export_bounds.count(dir) == 1);
  assert(export_data.count(dir) == 0);

  assert(dir->get_cum_auth_pins() == 0);

  // set ambiguous auth
  cache->adjust_subtree_auth(dir, dest, mds->get_nodeid());
  cache->verify_subtree_bounds(dir, export_bounds[dir]);
  
  // fill export message with cache data
  C_Contexts *fin = new C_Contexts;       // collect all the waiters
  int num_exported_inodes = encode_export_dir( export_data[dir], 
					       fin, 
					       dir,   // base
					       dir,   // recur start point
					       dest );
  
  // send the export data!
  MExportDir *req = new MExportDir(dir->ino());

  // export state
  req->set_dirstate( export_data[dir] );

  // add bounds to message
  for (set<CDir*>::iterator p = export_bounds[dir].begin();
       p != export_bounds[dir].end();
       ++p)
    req->add_export((*p)->ino());

  //s end
  mds->send_message_mds(req, dest, MDS_PORT_MIGRATOR);

  // queue up the finisher
  dir->add_waiter( CDir::WAIT_UNFREEZE, fin );

  // stats
  if (mds->logger) mds->logger->inc("ex");
  if (mds->logger) mds->logger->inc("iex", num_exported_inodes);

  cache->show_subtrees();
}


/** encode_export_inode
 * update our local state for this inode to export.
 * encode relevant state to be sent over the wire.
 * used by: encode_export_dir, file_rename (if foreign)
 */
void Migrator::encode_export_inode(CInode *in, bufferlist& enc_state, int new_auth)
{
  // tell (all) clients about migrating caps.. mark STALE
  for (map<int, Capability>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    dout(7) << "encode_export_inode " << *in << " telling client" << it->first << " stale caps" << endl;
    MClientFileCaps *m = new MClientFileCaps(in->inode, 
                                             it->second.get_last_seq(), 
                                             it->second.pending(),
                                             it->second.wanted(),
                                             MClientFileCaps::FILECAP_STALE);
    mds->messenger->send_message(m, mds->clientmap.get_inst(it->first),
				 0, MDS_PORT_CACHE);
  }

  // relax locks?
  if (!in->is_replicated())
    in->replicate_relax_locks();

  // add inode
  assert(!in->is_replica(mds->get_nodeid()));
  CInodeExport istate( in );
  istate._encode( enc_state );

  // we're export this inode; fix inode state
  dout(7) << "encode_export_inode " << *in << endl;
  
  if (in->is_dirty()) in->mark_clean();
  
  // clear/unpin cached_by (we're no longer the authority)
  in->clear_replicas();
  
  // twiddle lock states for auth -> replica transition
  // hard
  in->hardlock.clear_gather();
  if (in->hardlock.get_state() == LOCK_GLOCKR)
    in->hardlock.set_state(LOCK_LOCK);

  // file : we lost all our caps, so move to stable state!
  in->filelock.clear_gather();
  if (in->filelock.get_state() == LOCK_GLOCKR ||
      in->filelock.get_state() == LOCK_GLOCKM ||
      in->filelock.get_state() == LOCK_GLOCKL ||
      in->filelock.get_state() == LOCK_GLONERR ||
      in->filelock.get_state() == LOCK_GLONERM ||
      in->filelock.get_state() == LOCK_LONER)
    in->filelock.set_state(LOCK_LOCK);
  if (in->filelock.get_state() == LOCK_GMIXEDR)
    in->filelock.set_state(LOCK_MIXED);
  // this looks like a step backwards, but it's what we want!
  if (in->filelock.get_state() == LOCK_GSYNCM)
    in->filelock.set_state(LOCK_MIXED);
  if (in->filelock.get_state() == LOCK_GSYNCL)
    in->filelock.set_state(LOCK_LOCK);
  if (in->filelock.get_state() == LOCK_GMIXEDL)
    in->filelock.set_state(LOCK_LOCK);
    //in->filelock.set_state(LOCK_MIXED);
  
  // mark auth
  assert(in->is_auth());
  in->set_auth(false);
  in->replica_nonce = CInode::EXPORT_NONCE;
  
  // *** other state too?

  // move to end of LRU so we drop out of cache quickly!
  if (in->get_parent_dn()) 
    cache->lru.lru_bottouch(in->get_parent_dn());
}


int Migrator::encode_export_dir(list<bufferlist>& dirstatelist,
			      C_Contexts *fin,
			      CDir *basedir,
			      CDir *dir,
			      int newauth)
{
  int num_exported = 0;

  dout(7) << "encode_export_dir " << *dir << " " << dir->nitems << " items" << endl;
  
  assert(dir->get_projected_version() == dir->get_version());

  // dir 
  bufferlist enc_dir;
  
  CDirExport dstate(dir);
  dstate._encode( enc_dir );
  
  // release open_by 
  dir->clear_replicas();

  // mark
  assert(dir->is_auth());
  dir->state_clear(CDir::STATE_AUTH);
  dir->replica_nonce = CDir::NONCE_EXPORT;

  list<CDir*> subdirs;

  if (dir->is_hashed()) {
    // fix state
    dir->state_clear( CDir::STATE_AUTH );

  } else {
    
    if (dir->is_dirty())
      dir->mark_clean();
    
    // discard most dir state
    dir->state &= CDir::MASK_STATE_EXPORT_KEPT;  // i only retain a few things.
    
    // suck up all waiters
    list<Context*> waiting;
    dir->take_waiting(CDir::WAIT_ANY, waiting);    // all dir waiters
    fin->take(waiting);
    
    // inodes
    
    CDir_map_t::iterator it;
    for (it = dir->begin(); it != dir->end(); it++) {
      CDentry *dn = it->second;
      CInode *in = dn->get_inode();
      
      num_exported++;
      
      // -- dentry
      dout(7) << "encode_export_dir exporting " << *dn << endl;

      // name
      _encode(it->first, enc_dir);
      
      // state
      it->second->encode_export_state(enc_dir);

      // points to...

      // null dentry?
      if (dn->is_null()) {
        enc_dir.append("N", 1);  // null dentry
        assert(dn->is_sync());
        continue;
      }
      
      if (dn->is_remote()) {
        // remote link
        enc_dir.append("L", 1);  // remote link
        
        inodeno_t ino = dn->get_remote_ino();
        enc_dir.append((char*)&ino, sizeof(ino));
        continue;
      }
      
      // primary link
      // -- inode
      enc_dir.append("I", 1);    // inode dentry
      
      encode_export_inode(in, enc_dir, newauth);  // encode, and (update state for) export
      
      // directory?
      if (in->is_dir() && 
	  in->dir &&
	  !in->dir->state_test(CDir::STATE_EXPORTBOUND)) {
	// include nested subdir
	assert(in->dir->get_dir_auth().first == CDIR_AUTH_PARENT);
	subdirs.push_back(in->dir);  // it's ours, recurse (later)
      }
      
      // add to proxy
      //export_proxy_inos[basedir].push_back(in->ino());
      //in->state_set(CInode::STATE_PROXY);
      //in->get(CInode::PIN_PROXY);
      
      // waiters
      list<Context*> waiters;
      in->take_waiting(CInode::WAIT_ANY, waiters);
      fin->take(waiters);
    }
  }

  // add to dirstatelist
  bufferlist bl;
  dirstatelist.push_back( bl );
  dirstatelist.back().claim( enc_dir );

  // subdirs
  for (list<CDir*>::iterator it = subdirs.begin(); it != subdirs.end(); it++)
    num_exported += encode_export_dir(dirstatelist, fin, basedir, *it, newauth);

  return num_exported;
}


class C_MDS_ExportFinishLogged : public Context {
  Migrator *migrator;
  CDir *dir;
public:
  C_MDS_ExportFinishLogged(Migrator *m, CDir *d) : migrator(m), dir(d) {}
  void finish(int r) {
    migrator->export_logged_finish(dir);
  }
};


/*
 * i should get an export_ack from the export target.
 */
void Migrator::handle_export_ack(MExportDirAck *m)
{
  CDir *dir = cache->get_dir(m->get_ino());
  assert(dir);
  assert(dir->is_frozen_tree_root());  // i'm exporting!

  // yay!
  dout(7) << "handle_export_ack " << *dir << endl;

  export_warning_ack_waiting.erase(dir);
  
  export_state[dir] = EXPORT_LOGGINGFINISH;
  export_data.erase(dir);
  
  // log completion
  EExport *le = new EExport(dir);
  le->metablob.add_dir( dir, false );
  for (set<CDir*>::iterator p = export_bounds[dir].begin();
       p != export_bounds[dir].end();
       ++p) {
    CDir *bound = *p;
    le->get_bounds().insert(bound->ino());
    le->metablob.add_dir_context(bound);
    le->metablob.add_dir(bound, false);
  }

  // log export completion, then finish (unfreeze, trigger finish context, etc.)
  dir->get(CDir::PIN_LOGGINGEXPORTFINISH);
  mds->mdlog->submit_entry(le,
			   new C_MDS_ExportFinishLogged(this, dir));
  
  delete m;
}



/*
 * this happens if hte dest failes after i send teh export data but before it is acked
 * that is, we don't know they safely received and logged it, so we reverse our changes
 * and go on.
 */
void Migrator::export_reverse(CDir *dir)
{
  dout(7) << "export_reverse " << *dir << endl;
  
  assert(export_state[dir] == EXPORT_EXPORTING);
  assert(export_bounds.count(dir));
  assert(export_data.count(dir));
  
  // adjust auth, with possible subtree merge.
  cache->verify_subtree_bounds(dir, export_bounds[dir]);
  cache->adjust_subtree_auth(dir, mds->get_nodeid());
  cache->try_subtree_merge(dir);

  // unpin bounds
  for (set<CDir*>::iterator p = export_bounds[dir].begin();
       p != export_bounds[dir].end();
       ++p) {
    CDir *bd = *p;
    bd->put(CDir::PIN_EXPORTBOUND);
    bd->state_clear(CDir::STATE_EXPORTBOUND);
  }

  // re-import the metadata
  list<inodeno_t> imported_subdirs;
  int num_imported_inodes = 0;

  for (list<bufferlist>::iterator p = export_data[dir].begin();
       p != export_data[dir].end();
       ++p) {
    num_imported_inodes += 
      decode_import_dir(*p, 
                       export_peer[dir], 
                       dir,                 // import root
                       imported_subdirs,
		       0);
  }

  // process delayed expires
  cache->process_delayed_expire(dir);
  
  // tell peers
  //export_notify_abort(dir);
  
  // unfreeze
  dir->unfreeze_tree();

  // some clean up
  export_data.erase(dir);
  export_bounds.erase(dir);
  export_warning_ack_waiting.erase(dir);
  export_notify_ack_waiting.erase(dir);

  cache->show_cache();
}

/*
void Migrator::export_notify_abort(CDir* dir)
{
  dout(10) << "export_notify_abort " << *dir << endl;

  // send out notify(abort) to bystanders.  no ack necessary.
  for (set<int>::iterator p = export_notify_ack_waiting[dir].begin();
       p != export_notify_ack_waiting[dir].end();
       ++p) {
    MExportDirNotify *notify = new MExportDirNotify(dir->ino(), false,
						    pair<int,int>(mds->get_nodeid(), export_peer[dir]),
						    pair<int,int>(mds->get_nodeid(), CDIR_AUTH_UNKNOWN));
    notify->copy_exports(export_bounds[dir]);
    mds->send_message_mds(notify, *p, MDS_PORT_MIGRATOR);
  }
}
*/

/*
 * once i get the ack, and logged the EExportFinish(true),
 * send notifies (if any), otherwise go straight to finish.
 * 
 */
void Migrator::export_logged_finish(CDir *dir)
{
  dout(7) << "export_logged_finish " << *dir << endl;
  dir->put(CDir::PIN_LOGGINGEXPORTFINISH);

  cache->verify_subtree_bounds(dir, export_bounds[dir]);

  // send notifies
  int dest = export_peer[dir];

  for (set<int>::iterator p = export_notify_ack_waiting[dir].begin();
       p != export_notify_ack_waiting[dir].end();
       ++p) {
    MExportDirNotify *notify;
    if (mds->mdsmap->is_active_or_stopping(export_peer[dir])) 
      // dest is still alive.
      notify = new MExportDirNotify(dir->ino(), true,
				    pair<int,int>(mds->get_nodeid(), dest),
				    pair<int,int>(dest, CDIR_AUTH_UNKNOWN));
    else 
      // dest is dead.  bystanders will think i am only auth, as per mdcache->handle_mds_failure()
      notify = new MExportDirNotify(dir->ino(), true,
				    pair<int,int>(mds->get_nodeid(), CDIR_AUTH_UNKNOWN),
				    pair<int,int>(dest, CDIR_AUTH_UNKNOWN));

    notify->copy_exports(export_bounds[dir]);
    
    mds->send_message_mds(notify, *p, MDS_PORT_MIGRATOR);
  }

  // wait for notifyacks
  export_state[dir] = EXPORT_NOTIFYING;
  
  // no notifies to wait for?
  if (export_notify_ack_waiting[dir].empty())
    export_finish(dir);  // skip notify/notify_ack stage.
}

/*
 * warning:
 *  i'll get an ack from each bystander.
 *  when i get them all, do the export.
 * notify:
 *  i'll get an ack from each bystander.
 *  when i get them all, unfreeze and send the finish.
 */
void Migrator::handle_export_notify_ack(MExportDirNotifyAck *m)
{
  CInode *in = cache->get_inode(m->get_ino());
  CDir *dir = in ? in->dir : 0;

  assert(dir);
  int from = m->get_source().num();
    
  if (export_state.count(dir) && export_state[dir] == EXPORT_WARNING) {
    // exporting. process warning.
    dout(7) << "handle_export_notify_ack from " << m->get_source()
	    << ": exporting, processing warning on "
	    << *dir << endl;
    assert(export_warning_ack_waiting.count(dir));
    export_warning_ack_waiting[dir].erase(from);
    
    if (export_warning_ack_waiting[dir].empty()) 
      export_go(dir);     // start export.
  } 
  else if (export_state.count(dir) && export_state[dir] == EXPORT_NOTIFYING) {
    // exporting. process notify.
    dout(7) << "handle_export_notify_ack from " << m->get_source()
	    << ": exporting, processing notify on "
	    << *dir << endl;
    assert(export_notify_ack_waiting.count(dir));
    export_notify_ack_waiting[dir].erase(from);
    
    if (export_notify_ack_waiting[dir].empty())
      export_finish(dir);
  }
  else if (import_state.count(dir->ino()) && import_state[dir->ino()] == IMPORT_ABORTING) {
    // reversing import
    dout(7) << "handle_export_notify_ack from " << m->get_source()
	    << ": aborting import on "
	    << *dir << endl;
    assert(import_bystanders[dir].count(from));
    import_bystanders[dir].erase(from);
    if (import_bystanders[dir].empty()) {
      import_bystanders.erase(dir);
      import_reverse_unfreeze(dir);
    }
  }

  delete m;
}


void Migrator::export_finish(CDir *dir)
{
  dout(7) << "export_finish " << *dir << endl;

  if (export_state.count(dir) == 0) {
    dout(7) << "target must have failed, not sending final commit message.  export succeeded anyway." << endl;
    return;
  }

  // send finish/commit to new auth
  if (mds->mdsmap->is_active_or_stopping(export_peer[dir])) {
    mds->send_message_mds(new MExportDirFinish(dir->ino()), export_peer[dir], MDS_PORT_MIGRATOR);
  } else {
    dout(7) << "not sending MExportDirFinish, dest has failed" << endl;
  }
  
  // unfreeze
  dout(7) << "export_finish unfreezing" << endl;
  dir->unfreeze_tree();
  
  // unpin bounds
  for (set<CDir*>::iterator p = export_bounds[dir].begin();
       p != export_bounds[dir].end();
       ++p) {
    CDir *bd = *p;
    bd->put(CDir::PIN_EXPORTBOUND);
    bd->state_clear(CDir::STATE_EXPORTBOUND);
  }

  // adjust auth, with possible subtree merge.
  //  (we do this _after_ removing EXPORTBOUND pins, to allow merges)
  cache->adjust_subtree_auth(dir, export_peer[dir]);
  cache->try_subtree_merge(dir);
  
  // unpin path
  dout(7) << "export_finish unpinning path" << endl;
  vector<CDentry*> trace;
  cache->make_trace(trace, dir->inode);
  cache->path_unpin(trace, 0);

  // discard delayed expires
  cache->discard_delayed_expire(dir);

  // remove from exporting list, clean up state
  export_state.erase(dir);
  export_peer.erase(dir);
  export_bounds.erase(dir);
  export_notify_ack_waiting.erase(dir);
    
  // queue finishers
  mds->queue_finished(export_finish_waiters[dir]);
  export_finish_waiters.erase(dir);

  // stats
  //if (mds->logger) mds->logger->set("nex", cache->exports.size());

  cache->show_subtrees();

  // send pending import_maps?
  mds->mdcache->send_pending_import_maps();
}








// ==========================================================
// IMPORT


class C_MDC_ExportDirDiscover : public Context {
  Migrator *mig;
  MExportDirDiscover *m;
public:
  vector<CDentry*> trace;
  C_MDC_ExportDirDiscover(Migrator *mig_, MExportDirDiscover *m_) :
	mig(mig_), m(m_) {}
  void finish(int r) {
    CInode *in = 0;
    if (r >= 0) in = trace[trace.size()-1]->get_inode();
    mig->handle_export_discover_2(m, in, r);
  }
};  

void Migrator::handle_export_discover(MExportDirDiscover *m)
{
  assert(m->get_source().num() != mds->get_nodeid());

  dout(7) << "handle_export_discover on " << m->get_path() << endl;

  // must discover it!
  C_MDC_ExportDirDiscover *onfinish = new C_MDC_ExportDirDiscover(this, m);
  filepath fpath(m->get_path());
  cache->path_traverse(fpath, onfinish->trace, true,
		       m, new C_MDS_RetryMessage(mds,m),       // on delay/retry
		       MDS_TRAVERSE_DISCOVER,
		       onfinish);  // on completion|error
}

void Migrator::handle_export_discover_2(MExportDirDiscover *m, CInode *in, int r)
{
  // yay!
  if (in) {
    dout(7) << "handle_export_discover_2 has " << *in << endl;
  }

  if (r < 0 || !in->is_dir()) {
    dout(7) << "handle_export_discover_2 failed to discover or not dir " << m->get_path() << ", NAK" << endl;

    assert(0);    // this shouldn't happen if the auth pins his path properly!!!! 

    mds->send_message_mds(new MExportDirDiscoverAck(m->get_ino(), false),
			  m->get_source().num(), MDS_PORT_MIGRATOR);    
    delete m;
    return;
  }
  
  assert(in->is_dir());


  /*
  if (in->is_frozen()) {
    dout(7) << "frozen, waiting." << endl;
    in->add_waiter(CInode::WAIT_AUTHPINNABLE,
                   new C_MDS_RetryMessage(mds,m));
    return;
  }
  
  // pin auth too, until the import completes.
  in->auth_pin();
  */

  // pin inode in the cache (for now)
  in->get(CInode::PIN_IMPORTING);

  import_state[in->ino()] = IMPORT_DISCOVERED;
  import_peer[in->ino()] = m->get_source().num();

  // reply
  dout(7) << " sending export_discover_ack on " << *in << endl;
  mds->send_message_mds(new MExportDirDiscoverAck(in->ino()),
			m->get_source().num(), MDS_PORT_MIGRATOR);
  delete m;
}



void Migrator::handle_export_prep(MExportDirPrep *m)
{
  CInode *diri = cache->get_inode(m->get_ino());
  assert(diri);

  int oldauth = m->get_source().num();
  assert(oldauth != mds->get_nodeid());

  list<Context*> finished;

  // assimilate root dir.
  CDir *dir = diri->dir;
  if (dir) {
    dout(7) << "handle_export_prep on " << *dir << " (had dir)" << endl;

    if (!m->did_assim())
      m->get_dir(diri->ino())->update_dir(dir);
  } else {
    assert(!m->did_assim());

    // open dir i'm importing.
    diri->set_dir( new CDir(diri, mds->mdcache, false) );
    dir = diri->dir;
    m->get_dir(diri->ino())->update_dir(dir);
    
    dout(7) << "handle_export_prep on " << *dir << " (opening dir)" << endl;

    diri->take_waiting(CInode::WAIT_DIR, finished);
  }
  assert(dir->is_auth() == false);
  
  cache->show_subtrees();

  // assimilate contents?
  if (!m->did_assim()) {
    dout(7) << "doing assim on " << *dir << endl;
    m->mark_assim();  // only do this the first time!

    // move pin to dir
    diri->put(CInode::PIN_IMPORTING);
    dir->get(CDir::PIN_IMPORTING);  

    // change import state
    import_state[diri->ino()] = IMPORT_PREPPING;
    
    // bystander list
    import_bystanders[dir] = m->get_bystanders();
    dout(7) << "bystanders are " << import_bystanders[dir] << endl;

    // assimilate traces to exports
    for (list<CInodeDiscover*>::iterator it = m->get_inodes().begin();
         it != m->get_inodes().end();
         it++) {
      // inode
      CInode *in = cache->get_inode( (*it)->get_ino() );
      if (in) {
        (*it)->update_inode(in);
        dout(7) << " updated " << *in << endl;
      } else {
        in = new CInode(mds->mdcache, false);
        (*it)->update_inode(in);
        
        // link to the containing dir
        CInode *condiri = cache->get_inode( m->get_containing_dirino(in->ino()) );
        assert(condiri && condiri->dir);
		cache->add_inode( in );
        condiri->dir->add_dentry( m->get_dentry(in->ino()), in );
        
        dout(7) << "   added " << *in << endl;
      }
      
      assert( in->get_parent_dir()->ino() == m->get_containing_dirino(in->ino()) );
      
      // dir
      if (m->have_dir(in->ino())) {
        if (in->dir) {
          m->get_dir(in->ino())->update_dir(in->dir);
          dout(7) << " updated " << *in->dir << endl;
        } else {
          in->set_dir( new CDir(in, mds->mdcache, false) );
          m->get_dir(in->ino())->update_dir(in->dir);
          dout(7) << "   added " << *in->dir << endl;
          in->take_waiting(CInode::WAIT_DIR, finished);
        }
      }
    }

    // open export dirs/bounds?
    assert(import_bound_inos.count(diri->ino()) == 0);
    import_bound_inos[diri->ino()].clear();
    for (list<inodeno_t>::iterator it = m->get_exports().begin();
         it != m->get_exports().end();
         it++) {
      dout(7) << "  checking dir " << hex << *it << dec << endl;
      CInode *in = cache->get_inode(*it);
      assert(in);
      
      // note bound.
      import_bound_inos[dir->ino()].push_back(*it);

      if (!in->dir) {
        dout(7) << "  opening nested export on " << *in << endl;
        cache->open_remote_dir(in,
			       new C_MDS_RetryMessage(mds, m));
      }
    }
  } else {
    dout(7) << " not doing assim on " << *dir << endl;
  }
  

  // verify we have all exports
  int waiting_for = 0;
  for (list<inodeno_t>::iterator it = m->get_exports().begin();
       it != m->get_exports().end();
       it++) {
    inodeno_t ino = *it;
    CInode *in = cache->get_inode(ino);
    assert(in);
    if (in->dir) {
      if (!in->dir->state_test(CDir::STATE_IMPORTBOUND)) {
        dout(7) << "  pinning import bound " << *in->dir << endl;
        in->dir->get(CDir::PIN_IMPORTBOUND);
        in->dir->state_set(CDir::STATE_IMPORTBOUND);
	import_bounds[dir].insert(in->dir);
      } else {
        dout(7) << "  already pinned import bound " << *in << endl;
      }
    } else {
      dout(7) << "  waiting for nested export dir on " << *in << endl;
      waiting_for++;
    }
  }

  if (waiting_for) {
    dout(7) << " waiting for " << waiting_for << " nested export dir opens" << endl;
  } else {
    dout(7) << " all ready, noting auth and freezing import region" << endl;

    // note that i am an ambiguous auth for this subtree.
    // specify bounds, since the exporter explicitly defines the region.
    cache->adjust_bounded_subtree_auth(dir, import_bounds[dir], 
				       pair<int,int>(oldauth, mds->get_nodeid()));
    cache->verify_subtree_bounds(dir, import_bounds[dir]);
    
    // freeze.
    dir->_freeze_tree();
    
    // ok!
    dout(7) << " sending export_prep_ack on " << *dir << endl;
    mds->send_message_mds(new MExportDirPrepAck(dir->ino()),
			  m->get_source().num(), MDS_PORT_MIGRATOR);

    // note new state
    import_state[diri->ino()] = IMPORT_PREPPED;

    // done 
    delete m;
  }

  // finish waiters
  finish_contexts(finished, 0);
}




class C_MDS_ImportDirLoggedStart : public Context {
  Migrator *migrator;
  CDir *dir;
  int from;
  list<inodeno_t> imported_subdirs;
  list<inodeno_t> exports;
public:
  C_MDS_ImportDirLoggedStart(Migrator *m, CDir *d, int f, 
			     list<inodeno_t>& is, list<inodeno_t>& e) :
    migrator(m), dir(d), from(f) {
    imported_subdirs.swap(is);
    exports.swap(e);
  }
  void finish(int r) {
    migrator->import_logged_start(dir, from, imported_subdirs, exports);
  }
};

void Migrator::handle_export_dir(MExportDir *m)
{
  CDir *dir = cache->get_dir(m->get_ino());

  int oldauth = m->get_source().num();
  dout(7) << "handle_export_dir importing " << *dir << " from " << oldauth << endl;
  assert(dir->is_auth() == false);

  cache->show_subtrees();

  // start the journal entry
  EImportStart *le = new EImportStart(dir->ino(), m->get_exports());
  le->metablob.add_dir_context(dir);
  
  // adjust auth (list us _first_)
  cache->adjust_subtree_auth(dir, mds->get_nodeid(), oldauth);
  cache->verify_subtree_bounds(dir, import_bounds[dir]);

  // add this crap to my cache
  list<inodeno_t> imported_subdirs;
  int num_imported_inodes = 0;

  for (list<bufferlist>::iterator p = m->get_dirstate().begin();
       p != m->get_dirstate().end();
       ++p) {
    num_imported_inodes += 
      decode_import_dir(*p, 
                       oldauth, 
                       dir,                 // import root
                       imported_subdirs,
		       le);
  }
  dout(10) << " " << imported_subdirs.size() << " imported subdirs" << endl;
  dout(10) << " " << m->get_exports().size() << " imported nested exports" << endl;
  
  // include bounds in EImportStart
  for (set<CDir*>::iterator it = import_bounds[dir].begin();
       it != import_bounds[dir].end();
       it++) {
    CDir *bd = *it;

    // include bounding dirs in EImportStart
    // (now that the interior metadata is already in the event)
    le->metablob.add_dir(bd, false);
  }

  // adjust popularity
  mds->balancer->add_import(dir);

  dout(7) << "handle_export_dir did " << *dir << endl;

  // log it
  mds->mdlog->submit_entry(le,
			   new C_MDS_ImportDirLoggedStart(this, dir, m->get_source().num(), 
							  imported_subdirs, m->get_exports()));

  // note state
  import_state[dir->ino()] = IMPORT_LOGGINGSTART;

  // some stats
  if (mds->logger) {
    mds->logger->inc("im");
    mds->logger->inc("iim", num_imported_inodes);
    //mds->logger->set("nim", cache->imports.size());
  }

  delete m;
}


/*
 * note: this does teh full work of reversing and import and cleaning up
 *  state.  
 * called by both handle_mds_failure and by handle_import_map (if we are
 *  a survivor coping with an exporter failure+recovery).
 */
void Migrator::import_reverse(CDir *dir, bool fix_dir_auth)
{
  dout(7) << "import_reverse " << *dir << endl;

  // update auth, with possible subtree merge.
  if (fix_dir_auth) {
    assert(dir->is_subtree_root());
    cache->adjust_subtree_auth(dir, import_peer[dir->ino()]);
    cache->try_subtree_merge(dir);
  }

  // adjust auth bits.
  list<CDir*> q;
  q.push_back(dir);
  while (!q.empty()) {
    CDir *cur = q.front();
    q.pop_front();
    
    // dir
    assert(cur->is_auth());
    cur->state_clear(CDir::STATE_AUTH);
    cur->clear_replicas();
    if (cur->is_dirty())
      cur->mark_clean();

    CDir_map_t::iterator it;
    for (it = cur->begin(); it != cur->end(); it++) {
      CDentry *dn = it->second;

      // dentry
      dn->state_clear(CDentry::STATE_AUTH);
      dn->clear_replicas();
      if (dn->is_dirty()) 
	dn->mark_clean();

      // inode?
      if (dn->is_primary()) {
	CInode *in = dn->get_inode();
	in->state_clear(CDentry::STATE_AUTH);
	in->clear_replicas();
	if (in->is_dirty()) 
	  in->mark_clean();
	in->hardlock.clear_gather();
	in->filelock.clear_gather();

	// non-bounding dir?
	if (in->dir && 
	    !in->dir->state_test(CDir::STATE_IMPORTBOUND))
	  q.push_back(in->dir);
      }
    }
  }

  // log our failure
  mds->mdlog->submit_entry(new EImportFinish(dir,false));	// log failure

  // bystanders?
  if (import_bystanders[dir].empty()) {
    dout(7) << "no bystanders, finishing reverse now" << endl;
    import_reverse_unfreeze(dir);
  } else {
    // notify them; wait in aborting state
    dout(7) << "notifying bystanders of abort" << endl;
    import_notify_abort(dir);
    import_state[dir->ino()] = IMPORT_ABORTING;
  }
}

void Migrator::import_notify_abort(CDir *dir)
{
  dout(7) << "import_notify_abort " << *dir << endl;
  
  for (set<int>::iterator p = import_bystanders[dir].begin();
       p != import_bystanders[dir].end();
       ++p) {
    // NOTE: the bystander will think i am _only_ auth, because they will have seen
    // the exporter's failure and updated the subtree auth.  see mdcache->handle_mds_failure().
    MExportDirNotify *notify = 
      new MExportDirNotify(dir->ino(), true,
			   pair<int,int>(mds->get_nodeid(), CDIR_AUTH_UNKNOWN),
			   pair<int,int>(import_peer[dir->ino()], CDIR_AUTH_UNKNOWN));
    notify->copy_exports(import_bounds[dir]);
    mds->send_message_mds(notify, *p, MDS_PORT_MIGRATOR);
  }
}

void Migrator::import_reverse_unfreeze(CDir *dir)
{
  dout(7) << "import_reverse_unfreeze " << *dir << endl;

  // unfreeze
  dir->unfreeze_tree();

  // discard expire crap
  cache->discard_delayed_expire(dir);
  
  import_reverse_unpin(dir);
}

void Migrator::import_reverse_unpin(CDir *dir) 
{
  dout(7) << "import_reverse_unpin " << *dir << endl;

  // remove importing pin
  dir->put(CDir::PIN_IMPORTING);

  // remove bound pins
  for (set<CDir*>::iterator it = import_bounds[dir].begin();
       it != import_bounds[dir].end();
       it++) {
    CDir *bd = *it;
    bd->put(CDir::PIN_IMPORTBOUND);
    bd->state_clear(CDir::STATE_IMPORTBOUND);
  }

  // clean up
  import_state.erase(dir->ino());
  import_peer.erase(dir->ino());
  import_bound_inos.erase(dir->ino());
  import_bounds.erase(dir);
  import_bystanders.erase(dir);

  cache->show_subtrees();
  cache->show_cache();
}


void Migrator::import_logged_start(CDir *dir, int from,
				       list<inodeno_t> &imported_subdirs,
				       list<inodeno_t> &exports)
{
  dout(7) << "import_logged " << *dir << endl;

  // note state
  import_state[dir->ino()] = IMPORT_ACKING;

  // send notify's etc.
  dout(7) << "sending ack for " << *dir << " to old auth mds" << from << endl;
  mds->send_message_mds(new MExportDirAck(dir->inode->ino()),
			from, MDS_PORT_MIGRATOR);

  cache->show_subtrees();
}


void Migrator::handle_export_finish(MExportDirFinish *m)
{
  CDir *dir = cache->get_dir(m->get_ino());
  assert(dir);
  dout(7) << "handle_export_finish on " << *dir << endl;
  import_finish(dir);
  delete m;
}

void Migrator::import_finish(CDir *dir, bool now) 
{
  dout(7) << "import_finish on " << *dir << endl;

  // log finish
  mds->mdlog->submit_entry(new EImportFinish(dir, true));

  // remove pins
  dir->put(CDir::PIN_IMPORTING);

  for (set<CDir*>::iterator it = import_bounds[dir].begin();
       it != import_bounds[dir].end();
       it++) {
    CDir *bd = *it;

    // remove bound pin
    bd->put(CDir::PIN_IMPORTBOUND);
    bd->state_clear(CDir::STATE_IMPORTBOUND);
  }

  // unfreeze
  dir->unfreeze_tree();

  // adjust auth, with possible subtree merge.
  cache->verify_subtree_bounds(dir, import_bounds[dir]);
  cache->adjust_subtree_auth(dir, mds->get_nodeid());
  cache->try_subtree_merge(dir);
  
  // clear import state (we're done!)
  import_state.erase(dir->ino());
  import_peer.erase(dir->ino());
  import_bound_inos.erase(dir->ino());
  import_bounds.erase(dir);
  import_bystanders.erase(dir);

  // process delayed expires
  cache->process_delayed_expire(dir);

  // ok now finish contexts
  dout(5) << "finishing any waiters on imported data" << endl;
  dir->finish_waiting(CDir::WAIT_IMPORTED);

  // log it
  if (mds->logger) {
    //mds->logger->set("nex", cache->exports.size());
    //mds->logger->set("nim", cache->imports.size());
  }
  cache->show_subtrees();

  // is it empty?
  if (dir->get_size() == 0 &&
      !dir->inode->is_auth()) {
    // reexport!
    export_empty_import(dir);
  }
}


void Migrator::decode_import_inode(CDentry *dn, bufferlist& bl, int& off, int oldauth)
{  
  CInodeExport istate;
  off = istate._decode(bl, off);
  dout(15) << "got a cinodeexport " << endl;
  
  bool added = false;
  CInode *in = cache->get_inode(istate.get_ino());
  if (!in) {
    in = new CInode(mds->mdcache);
    added = true;
  } else {
    in->set_auth(true);
  }

  // state after link  -- or not!  -sage
  set<int> merged_client_caps;
  istate.update_inode(in, merged_client_caps);
 
  // link before state  -- or not!  -sage
  if (dn->inode != in) {
    assert(!dn->inode);
    dn->dir->link_inode(dn, in);
  }
 
  // add inode?
  if (added) {
    cache->add_inode(in);
    dout(10) << "added " << *in << endl;
  } else {
    dout(10) << "  had " << *in << endl;
  }
  
  
  // adjust replica list
  //assert(!in->is_replica(oldauth));  // not true on failed export
  in->add_replica( oldauth, CInode::EXPORT_NONCE );
  if (in->is_replica(mds->get_nodeid()))
    in->remove_replica(mds->get_nodeid());
  
  // twiddle locks
  // hard
  if (in->hardlock.get_state() == LOCK_GLOCKR) {
    in->hardlock.gather_set.erase(mds->get_nodeid());
    in->hardlock.gather_set.erase(oldauth);
    if (in->hardlock.gather_set.empty())
      mds->locker->inode_hard_eval(in);
  }

  // caps
  for (set<int>::iterator it = merged_client_caps.begin();
       it != merged_client_caps.end();
       it++) {
    MClientFileCaps *caps = new MClientFileCaps(in->inode,
                                                in->client_caps[*it].get_last_seq(),
                                                in->client_caps[*it].pending(),
                                                in->client_caps[*it].wanted(),
                                                MClientFileCaps::FILECAP_REAP);
    caps->set_mds( oldauth ); // reap from whom?
    mds->messenger->send_message(caps, 
				 mds->clientmap.get_inst(*it),
				 0, MDS_PORT_CACHE);
  }

  // filelock
  if (!in->filelock.is_stable()) {
    // take me and old auth out of gather set
    in->filelock.gather_set.erase(mds->get_nodeid());
    in->filelock.gather_set.erase(oldauth);
    if (in->filelock.gather_set.empty())  // necessary but not suffient...
      mds->locker->inode_file_eval(in);    
  }
}


int Migrator::decode_import_dir(bufferlist& bl,
			       int oldauth,
			       CDir *import_root,
			       list<inodeno_t>& imported_subdirs,
			       EImportStart *le)
{
  int off = 0;
  
  // set up dir
  CDirExport dstate;
  off = dstate._decode(bl, off);
  
  CInode *diri = cache->get_inode(dstate.get_ino());
  assert(diri);
  CDir *dir = diri->get_or_open_dir(mds->mdcache);
  assert(dir);
  
  dout(7) << "decode_import_dir " << *dir << endl;

  // add to list
  if (dir != import_root)
    imported_subdirs.push_back(dir->ino());

  // assimilate state
  dstate.update_dir( dir );

  // mark  (may already be marked from get_or_open_dir() above)
  if (!dir->is_auth())
    dir->state_set(CDir::STATE_AUTH);

  // adjust replica list
  //assert(!dir->is_replica(oldauth));    // not true on failed export
  dir->add_replica(oldauth);
  if (dir->is_replica(mds->get_nodeid()))
    dir->remove_replica(mds->get_nodeid());

  // add to journal entry
  if (le) 
    le->metablob.add_dir(dir, true);  // Hmm: false would be okay in some cases

  int num_imported = 0;

  if (dir->is_hashed()) {

    // do nothing; dir is hashed
  } else {
    // take all waiters on this dir
    // NOTE: a pass of imported data is guaranteed to get all of my waiters because
    // a replica's presense in my cache implies/forces it's presense in authority's.
    list<Context*> waiters;
    
    dir->take_waiting(CDir::WAIT_ANY, waiters);
    for (list<Context*>::iterator it = waiters.begin();
         it != waiters.end();
         it++) 
      import_root->add_waiter(CDir::WAIT_IMPORTED, *it);
    
    dout(15) << "doing contents" << endl;
    
    // contents
    long nden = dstate.get_nden();

    for (; nden>0; nden--) {
      
      num_imported++;
      
      // dentry
      string dname;
      _decode(dname, bl, off);
      dout(15) << "dname is " << dname << endl;

      CDentry *dn = dir->lookup(dname);
      if (!dn)
        dn = dir->add_dentry(dname);  // null

      // decode state
      dn->decode_import_state(bl, off, oldauth, mds->get_nodeid());

      // points to...
      char icode;
      bl.copy(off, 1, &icode);
      off++;
      
      if (icode == 'N') {
        // null dentry
        assert(dn->is_null());  
        
        // fall thru
      }
      else if (icode == 'L') {
        // remote link
        inodeno_t ino;
        bl.copy(off, sizeof(ino), (char*)&ino);
        off += sizeof(ino);
        dir->link_inode(dn, ino);
      }
      else if (icode == 'I') {
        // inode
        decode_import_inode(dn, bl, off, oldauth);
      }

      // add dentry to journal entry
      if (le) 
	le->metablob.add_dentry(dn, true);  // Hmm: might we do dn->is_dirty() here instead?  
    }

  }

  dout(7) << "decode_import_dir done " << *dir << endl;
  return num_imported;
}





// authority bystander

void Migrator::handle_export_warning(MExportDirWarning *m)
{
  CDir *dir = cache->get_dir(m->get_ino());

  int oldauth = m->get_source().num();
  int newauth = m->get_new_dir_auth();
  if (dir) {
    dout(7) << "handle_export_warning mds" << oldauth
	    << " -> mds" << newauth
	    << " on " << *dir << endl;
    cache->adjust_subtree_auth(dir, oldauth, newauth);
    // verify?
  } else {
    dout(7) << "handle_export_warning on dir " << m->get_ino() << ", acking" << endl;
  }
  
  // send the ack
  mds->send_message_mds(new MExportDirWarningAck(m->get_ino()),
			m->get_source().num(), MDS_PORT_MIGRATOR);

  delete m;

}


void Migrator::handle_export_notify(MExportDirNotify *m)
{
  CDir *dir = cache->get_dir(m->get_ino());

  int from = m->get_source().num();
  pair<int,int> old_auth = m->get_old_auth();
  pair<int,int> new_auth = m->get_new_auth();
  
  if (!dir) {
    dout(7) << "handle_export_notify " << old_auth << " -> " << new_auth
	    << " on missing dir " << m->get_ino() << endl;
  } else if (dir->authority() != old_auth) {
    dout(7) << "handle_export_notify old_auth was " << dir->authority() 
	    << " != " << old_auth << " -> " << new_auth
	    << " on " << *dir << endl;
  } else {
    dout(7) << "handle_export_notify " << old_auth << " -> " << new_auth
	    << " on " << *dir << endl;
    // adjust auth
    cache->adjust_bounded_subtree_auth(dir, m->get_exports(), new_auth);
    
    // induce a merge?
    cache->try_subtree_merge(dir);
  }
  
  // send ack
  if (m->wants_ack()) {
    mds->send_message_mds(new MExportDirNotifyAck(m->get_ino()),
			  from, MDS_PORT_MIGRATOR);
  } else {
    // aborted.  no ack.
    dout(7) << "handle_export_notify no ack requested" << endl;
  }
  
  delete m;
}




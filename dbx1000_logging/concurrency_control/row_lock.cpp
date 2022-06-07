#include "row.h"
#include "txn.h"
#include "helper.h"
#include "row_lock.h"
#include "mem_alloc.h"
#include "manager.h"

#if CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE || CC_ALG == DL_DETECT

#define CONFLICT(a, b) (a != LOCK_NONE_T && b != LOCK_NONE_T) && (a==LOCK_EX_T || b==LOCK_EX_T)

void Row_lock::init(row_t * row) {
	_row = row;
	/*
	owners = NULL;
	waiters_head = NULL;
	waiters_tail = NULL;
	*/
	// owner_cnt = 0;
	//waiter_cnt = 0;

	
#if !USE_LOCKTABLE
	latch = new pthread_mutex_t;
	pthread_mutex_init(latch, NULL);
	blatch = false;
#endif
	
	lock_type = LOCK_NONE_T;
	ownerCounter = 0;

}

RC Row_lock::lock_get(lock_t type, txn_man * txn) {
	uint64_t *txnids = NULL;
	int txncnt = 0;
	return lock_get(type, txn, txnids, txncnt);
}

RC Row_lock::lock_get(lock_t type, txn_man * txn, uint64_t* &txnids, int &txncnt) {
	uint64_t starttime = get_sys_clock();
	assert (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE);
	RC rc;
	//int part_id =_row->get_part_id();
#if !USE_LOCKTABLE  // otherwise we don't need a latch here.
	if (g_central_man)
		glob_manager->lock_row(_row);
	else 
		pthread_mutex_lock( latch );
#endif
	//cout << "Owner_cnt " << owner_cnt << endl;
	//assert(owner_cnt <= g_thread_cnt);
	//assert(waiter_cnt < g_thread_cnt);
/*#if DEBUG_ASSERT
	if (owners != NULL)
		assert(lock_type == owners->type); 
	else 
		assert(lock_type == LOCK_NONE_T);
	LockEntry * en = owners;
	UInt32 cnt = 0;
	while (en) {
		assert(en->txn->get_thd_id() != txn->get_thd_id());
		cnt ++;
		en = en->next;
	}
	assert(cnt == owner_cnt);
	en = waiters_head;
	cnt = 0;
	while (en) {
		cnt ++;
		en = en->next;
	}
	assert(cnt == waiter_cnt);
#endif*/
	// what is this assertion here?
	//assert(lock_type == LOCK_NONE_T);
	// IMPORTANT: for simplicity, 
	// we assume that if a transaction reads and writes to the same tuple
	// it will first acquired the exclusive lock.
	bool conflict = CONFLICT(lock_type, type);// conflict_lock(lock_type, type);
	if (conflict) { 
		// Cannot be added to the owner list.
		if (CC_ALG == NO_WAIT) {
			rc = Abort;
		}
	} else {
		INC_INT_STATS(time_debug6, get_sys_clock() - starttime);
		//LockEntry entry = LockEntry {type, txn}; 
		//++ owner_cnt;
		//_owners.push_back(entry);
		ownerCounter ++;
		//printf("[%" PRIu64 "] Pushed %" PRIu64 " with type %d, current locktype %d\n", (uint64_t) _row, (uint64_t) txn, type, lock_type);
		lock_type = type;
        rc = RCOK;
	}
#if !USE_LOCKTABLE  // otherwise we don't need a latch here.
	if (g_central_man)
		glob_manager->release_row(_row);
	else
		pthread_mutex_unlock( latch );
#endif
	INC_INT_STATS(time_debug7, get_sys_clock() - starttime);
	return rc;
}


RC Row_lock::lock_release(txn_man * txn) {	

#if !USE_LOCKTABLE  // otherwise we don't need a latch here.
	if (g_central_man)
		glob_manager->lock_row(_row);
	else 
		pthread_mutex_lock( latch );
#endif
// Try to find the entry in the owners
//	LockEntry * en = owners;
//	LockEntry * prev = NULL;
	//bool found = false;
	// TODO. right now, just sequentially search the vector of _owners
	/*
	for (vector<LockEntry>::iterator it = _owners.begin(); it != _owners.end(); it ++) {
		if (it->txn == txn) {
			//entry = &(*it);
			found = true;
			_owners.erase(it);
			//printf("[%" PRIu64 "] Erased %" PRIu64 " with type %d\n", (uint64_t) _row, (uint64_t) txn, it->type);
			//owner_cnt --;
			break;
		}
	}
	*/
	ownerCounter --;
#if (CC_ALG == NO_WAIT)
	//assert(found);
#endif
	//if (found) {
		// TODO. continue from here.  
		//_row->set_last_writer( txn-> );
	//}
	if (ownerCounter == 0)//(_owners.empty())
		lock_type = LOCK_NONE_T;
/*	while (en != NULL && en->txn != txn) {
		prev = en;
		en = en->next;
	}
	if (en) { // find the entry in the owner list
		if (prev) prev->next = en->next;
		else owners = en->next;
		return_entry(en);
		owner_cnt --;
		if (owner_cnt == 0)
			lock_type = LOCK_NONE_T;
	} else {
		// Not in owners list, try waiters list.
		en = waiters_head;
		while (en != NULL && en->txn != txn)
			en = en->next;
		ASSERT(en);
		LIST_REMOVE(en);
		if (en == waiters_head)
			waiters_head = en->next;
		if (en == waiters_tail)
			waiters_tail = en->prev;
		return_entry(en);
		waiter_cnt --;
	}
*/
//	if (owner_cnt == 0)
//		ASSERT(lock_type == LOCK_NONE_T);
//#if DEBUG_ASSERT && CC_ALG == WAIT_DIE 
//		for (en = waiters_head; en != NULL && en->next != NULL; en = en->next)
//			assert(en->next->txn->get_ts() < en->txn->get_ts());
//#endif

/*	LockEntry * entry;
	// If any waiter can join the owners, just do it!
	while (waiters_head && !conflict_lock(lock_type, waiters_head->type)) {
		LIST_GET_HEAD(waiters_head, waiters_tail, entry);
		STACK_PUSH(owners, entry);
		owner_cnt ++;
		waiter_cnt --;
		ASSERT(entry->txn->lock_ready == false);
		entry->txn->lock_ready = true;
		lock_type = entry->type;
	} 
	ASSERT((owners == NULL) == (owner_cnt == 0));
*/
#if !USE_LOCKTABLE  // otherwise we don't need a latch here.
	if (g_central_man)
		glob_manager->release_row(_row);
	else
		pthread_mutex_unlock( latch );
#endif
	return RCOK;
}

bool Row_lock::conflict_lock(lock_t l1, lock_t l2) {
	if (l1 == LOCK_NONE_T || l2 == LOCK_NONE_T)
	{
		return false;
	}
  else if (l1 == LOCK_EX_T || l2 == LOCK_EX_T)
	{
        return true;
	}
	else
	{
		return false;
	}
}

LockEntry * Row_lock::get_entry() {
	LockEntry * entry = (LockEntry *) 
		mem_allocator.alloc(sizeof(LockEntry), _row->get_part_id());
	return entry;
}
void Row_lock::return_entry(LockEntry * entry) {
	mem_allocator.free(entry, sizeof(LockEntry));
}

#endif

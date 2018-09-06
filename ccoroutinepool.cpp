#include "ccoroutinepool.h"
#include <time.h>
#include <stdarg.h>

pCCoroutineMalloc g_malloc = malloc;
pCCoroutineFree g_free = free;
pCCoroutineLog g_log = nullptr;

template<class... _Types>
void CCoroutinePoolLog(CCoroutineLogStatus status, const char* pLog, _Types&&... _Args) {
    if (g_log) {
        char szBuf[256];
        ccsnprintf(szBuf, 256, pLog, std::forward<_Types>(_Args)...);
        g_log(status, szBuf);
    }
    else {
        printf(pLog, std::forward<_Types>(_Args)...);
    }
}
void CCorutineInitFunc(pCCoroutineMalloc pMalloc, pCCoroutineFree pFree, pCCoroutineLog pLog) {
    if (pMalloc && pFree) {
        g_malloc = pMalloc;
        g_free = pFree;
    }
    if (pLog) {
        g_log = pLog;
    }
}
/////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef _MSC_VER
extern "C"
{
    extern void coctx_swap(coctx_t *, coctx_t*);
}
#elif defined(__GNUC__)
extern "C" {
    extern void coctx_swap(coctx_t *, coctx_t*) asm("coctx_swap");
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////////////
CCORUTINETLS_Key CreateTLSKey() {
#ifdef _MSC_VER
    return TlsAlloc();
#else
    CCORUTINETLS_Key key;
    pthread_key_create(&key, nullptr);
    return key;
#endif
}

CCORUTINETLS_Key CCorutineThreadData::m_tlsKey = CreateTLSKey();

CCorutineThreadData::CCorutineThreadData() {
    m_dwThreadID = CCorutineGetThreadID();
#ifdef _MSC_VER
    TlsSetValue(m_tlsKey, this);
#else
    pthread_setspecific(m_tlsKey, this);
#endif
}
CCorutineThreadData* CCorutineThreadData::GetTLSValue(){
#ifdef _MSC_VER
    return (CCorutineThreadData*)TlsGetValue(m_tlsKey);
#else
    return (CCorutineThreadData*)pthread_getspecific(m_tlsKey);
#endif
}

void* CCorutineThreadData::operator new(size_t nSize) {
    return g_malloc(nSize);
}
void CCorutineThreadData::operator delete(void* p) {
    g_free(p);
}
/////////////////////////////////////////////////////////////////////////////////////////////
CCorutinePoolMgr CCorutinePool::m_poolMgr;
bool CCorutinePool::CheckCorutinePoolMgrCorrect() {
    return m_poolMgr.CheckCorutinePoolMgrCorrect();
}
CCorutinePool::CCorutinePool() {
    m_usRunCorutineStack = 0;
    memset(m_pStackRunCorutine, 0, sizeof(CCorutine*) * CorutinePlus_Max_Stack);
    m_pStackRunCorutine[m_usRunCorutineStack++] = &m_main;
    m_main.m_pRunPool = this;

    m_cap = 0;
    m_queue = nullptr;
    m_nCorutineSize = 0;
    m_nCreateTimes = 0;
    m_nLimitSize = 1024;

    m_bInit = false;
}

CCorutinePool::~CCorutinePool() {
    if (m_queue) {
        if (m_nCorutineSize > 0) {
            m_poolMgr.ReleaseCorutineToGlobal(m_queue, m_nCorutineSize);
        }
        g_free(m_queue);
    }
}

void* CCorutinePool::operator new(size_t nSize) {
    return g_malloc(nSize);
}
void CCorutinePool::operator delete(void* p) {
    g_free(p);
}

void CCorutinePool::AddNewQueue() {
    CCorutine** new_queue = (CCorutine**)g_malloc(m_cap * 2 * sizeof(CCorutine*));
    memcpy(new_queue, m_queue, m_cap * sizeof(CCorutine*));
    m_cap *= 2;
    g_free(m_queue);
    m_queue = new_queue;
}

bool CCorutinePool::InitCorutine(uint32_t nDefaultSize, uint32_t nLimitCorutineCount) {
    if (m_bInit)
        return true;
    if (nDefaultSize == 0 || nLimitCorutineCount < nDefaultSize)
        return false;
    m_bInit = true;
    m_nLimitSize = nLimitCorutineCount;
    m_cap = nDefaultSize;
    m_queue = (CCorutine**)g_malloc(m_cap * sizeof(CCorutine*));

    for (uint32_t i = m_poolMgr.GetCorutineFromGlobal(m_queue, m_cap); i < nDefaultSize; i++) {
        m_queue[i] = CreateCorutine();
    }
    m_nCorutineSize = nDefaultSize;
    return true;
}

CCorutine* CCorutinePool::GetCorutine() {
    CCorutine* pRet = nullptr;
    if (CCoroutineUnLikely(m_nCorutineSize == 0)) {
        do {
            m_nCorutineSize = m_poolMgr.GetCorutineFromGlobal(m_queue, m_cap > 256 ? 128 : m_cap / 2);
            if (m_nCorutineSize > 0) {
                break;
            }
            if (m_nCreateTimes < m_nLimitSize) {
                return CreateCorutine();
            }
            else {
#ifdef _MSC_VER
                SwitchToThread();
#else
                std::this_thread::yield();
#endif
                m_nSleepTimes++;
            }
        } while (true);
    }
    pRet = m_queue[--m_nCorutineSize];
    return pRet;
}

void CCorutinePool::ReleaseCorutine(CCorutine* pPTR) {
    if (CCoroutineUnLikely(m_nCorutineSize == m_cap)) {
        // 1/4 to global 
        uint16_t nSize = m_nCorutineSize > 2048 ? 512 : m_nCorutineSize / 4;
        m_nCorutineSize -= nSize;
        m_poolMgr.ReleaseCorutineToGlobal(m_queue + m_nCorutineSize, nSize);
        // * 2
        if (m_cap < m_nLimitSize) {
            AddNewQueue();
        }
    }
    m_queue[m_nCorutineSize++] = pPTR;
}

void CCorutinePool::ResumeFunc(CCorutine* pNext) {
    CCorutine* pRunCorutine = m_pStackRunCorutine[m_usRunCorutineStack - 1];
    
    m_pStackRunCorutine[m_usRunCorutineStack++] = pNext;
    pNext->m_state = CoroutineState_Running;
    coctx_swap(&pRunCorutine->m_ctx, &pNext->m_ctx);
}

void CCorutinePool::YieldFunc(CCorutine* pCorutine) {
    m_usRunCorutineStack--;
#ifdef _DEBUG
    if (m_usRunCorutineStack <= 0) {
        assert(0);
    }
#endif
    CCorutine* pChange = m_pStackRunCorutine[m_usRunCorutineStack - 1];
    pCorutine->m_state = CoroutineState_Suspend;
    //no need to copy stack
    coctx_swap(&pCorutine->m_ctx, &pChange->m_ctx);
}

void CCorutinePool::FinishFunc(CCorutine* pCorutine) {
    pCorutine->m_state = CoroutineState_Death;
    m_usRunCorutineStack--;
    //ret p
    ReleaseCorutine(pCorutine);

    do {
        coctx_swap(&pCorutine->m_ctx, &m_pStackRunCorutine[m_usRunCorutineStack - 1]->m_ctx);
        assert(0);
        CCoroutinePoolLog(CCoroutineLogStatus_Error, ("error, finish must be no resume..."));
    } while (true);
}

CCorutine* CCorutinePool::CreateCorutine() {
    CCorutine* pRet = ConstructCorutine();
    m_nCreateTimes++;
    m_poolMgr.CreateCorutine(pRet);
    return pRet;
}

CCorutine* CCorutinePool::ConstructCorutine() {
    return new CCorutineWithStack();
}
//////////////////////////////////////////////////////////////////////////////////////////////////////
CCorutinePoolMgr::CCorutinePoolMgr() {
    m_cap = 32;
    m_tail = 0;
    m_queue = (CCorutine**)g_malloc(m_cap * sizeof(CCorutine*));

    m_CheckTail = 0;
    m_checkQueue = nullptr;
    m_checkCap = 0;

    m_moreCap = m_cap;
    m_moreTail = 0;
    m_moreQueue = (CCorutine**)g_malloc(m_moreCap * sizeof(CCorutine*));
}

CCorutinePoolMgr::~CCorutinePoolMgr() {
    for (uint32_t i = 0; i < m_tail; i++) {
        delete m_queue[i];
    }
    if (m_queue){
        g_free(m_queue);
    }
}

void* CCorutinePoolMgr::operator new(size_t nSize) {
    return g_malloc(nSize);
}
void CCorutinePoolMgr::operator delete(void* p) {
    g_free(p);
}

void CCorutinePoolMgr::CreateCorutine(CCorutine* p) {
    while (m_nLock.exchange(true, std::memory_order_acquire)) {
        _mm_pause();
    }
    m_queue[m_tail++] = p;
    if (CCoroutineUnLikely(m_tail >= m_cap)) {
        AddNewQueue();
    }
    m_nLock.store(false, std::memory_order_release);
}

void CCorutinePoolMgr::AddNewQueue() {
    CCorutine** new_queue = (CCorutine**)g_malloc(m_cap * 2 * sizeof(CCorutine*));
    memcpy(new_queue, m_queue, m_cap * sizeof(CCorutine*));
    m_tail = m_cap;
    m_cap *= 2;
    g_free(m_queue);
    m_queue = new_queue;
}

//! check 
void CCorutinePoolMgr::CheckAllCorutine() {
    if (m_tail == 0)
        return;
    if (CCoroutineUnLikely(m_tail != m_CheckTail)) {
        while (m_nLock.exchange(true, std::memory_order_acquire)) {
            _mm_pause();
        }
        if (m_checkCap != m_cap) {
            m_checkCap = m_cap;
            if (m_checkQueue) {
                g_free(m_checkQueue);
            }
            m_checkQueue = (CCorutine**)g_malloc(m_checkCap * sizeof(CCorutine*));
        }
        m_CheckTail = m_tail;
        memcpy(m_checkQueue, m_queue, m_CheckTail * sizeof(CCorutine*));
        m_nLock.store(false, std::memory_order_release);
    }
    time_t nowTime = time(nullptr);
    for (uint32_t i = 0; i < m_CheckTail; i++) {
        if (CCoroutineUnLikely(m_checkQueue[i]->IsCoroutineError(nowTime))) {
            char szBuf[64];
            m_checkQueue[i]->GetStatus(szBuf, 64);
            CCoroutinePoolLog(CCoroutineLogStatus_Error, ("Corutine check error, maybe loop or no wakeup state(%d) (%s)", m_checkQueue[i]->GetCoroutineState(), szBuf));
        }
    }
}

void CCorutinePoolMgr::ReleaseCorutineToGlobal(CCorutine* pMore[], uint32_t nCount) {
    while (m_nLockMore.exchange(true, std::memory_order_acquire)) {
        _mm_pause();
    }
    m_moreTail += nCount;
    if (m_moreTail > m_moreCap) {
        do {
            m_moreCap *= 2;
        } while (m_moreCap <= m_moreTail);
        auto* moreQueue = (CCorutine**)g_malloc(m_moreCap * sizeof(CCorutine*));
        memcpy(moreQueue, m_moreQueue, (m_moreTail - nCount) * sizeof(CCorutine*));
        g_free(m_moreQueue);
        m_moreQueue = moreQueue;
    }
    memcpy(m_moreQueue + m_moreTail - nCount, pMore, nCount * sizeof(CCorutine*));
    m_nLockMore.store(false, std::memory_order_release);
}

uint32_t CCorutinePoolMgr::GetCorutineFromGlobal(CCorutine* pMore[], uint32_t nCount) {
    while (m_nLockMore.exchange(true, std::memory_order_acquire)) {
        _mm_pause();
    }
    if (m_moreTail == 0) {
        m_nLockMore.store(false, std::memory_order_release);
        return 0;
    }
    int nRet = nCount;
    if (m_moreTail < nCount) {
        nRet = m_moreTail;
    }
    m_moreTail -= nRet;
    memcpy(pMore, m_moreQueue + m_moreTail, nRet * sizeof(CCorutine*));
    m_nLockMore.store(false, std::memory_order_release);
    return nRet;
}

bool CCorutinePoolMgr::CheckCorutinePoolMgrCorrect() {
    if (m_tail == m_moreTail) {
        return true;
    }
    return false;
}

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "netplay_content_gate.h"
#include <assert.h>
static const char *fp="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static void send(NpContentGate *a,NpContentGate *b,int chunk) {
    np_content_note(b,a->local,chunk|(a->mismatch?0x80:0),a->words[chunk*2],a->words[chunk*2+1],a->matched);
}
int main(void) {
    NpContentGate a,b,c;int i;
    assert(np_content_init(&a,fp,3,0));assert(np_content_init(&b,fp,3,1));
    assert(!np_content_ready(&a));
    // Reordering, duplicate and missing fragment: never boot on a partial hash.
    send(&a,&b,3);send(&a,&b,3);send(&a,&b,1);send(&a,&b,0);
    assert(!np_content_ready(&b));send(&a,&b,2);assert(!np_content_ready(&b));
    for(i=0;i<4;i++)send(&b,&a,i);
    assert(np_content_ready(&a));assert(!np_content_ready(&b));
    // Final acknowledgement can be lost; retransmission unblocks the peer.
    send(&a,&b,0);assert(np_content_ready(&b));
    // A different byte in every quarter is detected and propagates to the host.
    for(i=0;i<8;i++) {
        assert(np_content_init(&a,fp,3,0));assert(np_content_init(&b,fp,3,1));
        b.words[i]^=1;send(&b,&a,i/2);assert(a.mismatch && !np_content_ready(&a));
        send(&a,&b,0);assert(b.mismatch && !np_content_ready(&b));
    }
    // Sparse occupied seats wait for all participants, but ignore absent seats.
    assert(np_content_init(&a,fp,0x105,0));assert(np_content_init(&b,fp,0x105,2));
    assert(np_content_init(&c,fp,0x105,8));
    for(i=0;i<4;i++){send(&a,&b,i);send(&b,&a,i);}
    assert(!np_content_ready(&a));
    for(i=0;i<4;i++){send(&a,&c,i);send(&b,&c,i);send(&c,&a,i);send(&c,&b,i);}
    send(&a,&b,0);send(&b,&a,0);send(&a,&c,0);send(&b,&c,0);
    assert(np_content_ready(&a)&&np_content_ready(&b)&&np_content_ready(&c));
    assert(!np_content_init(&a,"wrong",3,0));assert(!np_content_init(&a,fp,3,9));
    assert(np_content_init(&a,"",3,0)&&np_content_ready(&a));
    return 0;
}

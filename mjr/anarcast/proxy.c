#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include "anarcast.h"
#include "sha.c"

// how many graphs do we have?
#define GRAPHCOUNT    512

// how many blocks should be transfer at a time?
#define CONCURRENCY   8

// a teenage mutant ninja graph (tm)
struct graph {
    unsigned short dbc; // data block count
    unsigned short cbc; // check block count
    unsigned char *graph; // array of bits
};

// a node of our lovely AVL tree
struct node {
    unsigned int addr;
    char hash[HASHLEN];
    struct node *left, *right;
    unsigned char heightdiff;
};

// load our evil mutant graphs from the graph file
void load_graphs ();

// the graphs themselves. graphs[0] = 1 data block, and so on
struct graph graphs[GRAPHCOUNT];

// our lovely AVL tree
struct node *tree;

// our inform server hostname
char *inform_server;

// connect to the inform server and populate our lovely AVL tree
void inform ();

// read the transaction type, call the right function, and close the connection
void * run_thread (void *arg);

// mandatory variadic function
void alert (const char *s, ...);

// check if bit (db * cb) is set in g->graph
int is_set (struct graph *g, int db, int cb);

// read data, send back key, insert blocks, etc
void insert (int c);

// insert blocks that aren't set true at mask[block]
void do_insert (const char *blocks, const char *mask, int blockcount, int blocksize, const char *hashes);

// read key from client, download blocks, reconstruct data, insert reconstructed parts, etc
void request (int c);

// download all the blocks i can get, set mask[block] = 1 for each begot block
void do_request (char *blocks, char *mask, int blockcount, int blocksize, const char *hashes);

// add a reference to our lovely AVL tree. it better not be a duplicate!
void addref (unsigned int addr);

// remove a reference to our lovely AVL tree. it better be there!
void rmref (unsigned int addr);

// return the address of the proper host for hash
unsigned int route (const char hash[HASHLEN]);

int
main (int argc, char **argv)
{
    int l, c;
    pthread_t t;
    
    if (argc != 2) {
	fprintf(stderr, "Usage: %s <inform server>\n", argv[0]);
        exit(2);
    }
    
    chdir_to_home();
    load_graphs();
    inform((inform_server = argv[1]));
    l = listening_socket(PROXY_SERVER_PORT);
    
    // accept connections and spawn a thread for each
    for (;;)
	if ((c = accept(l, NULL, 0)) != -1) {
	    int *i = malloc(4);
	    *i = c;
	    if (pthread_create(&t, NULL, run_thread, i) != 0)
		die("pthread_create() failed");
	    if (pthread_detach(t) != 0)
		die("pthread_detach() failed");
	}
}

void *
run_thread (void *arg)
{
    int c = *(int*)arg;
    char d;

    // read transaction type, call handler
    if (read(c, &d, 1) == 1) {
        if (d == 'r') request(c);
        if (d == 'i') insert(c);
    }

    if (close(c) == -1)
	die("close() failed");
    free(arg);
    pthread_exit(NULL);
}

void
alert (const char *s, ...)
{
    va_list args;
    va_start(args, s);
    vprintf(s, args);
    // all this for a fucking newline
    printf("\n");
    fflush(stdout);
    va_end(args);
}

//=== graph =================================================================

void
load_graphs ()
{
    int i, n;
    char *data;
    struct stat s;
    
    if (stat("graphs", &s) == -1)
	die("Can't stat graphs file");
    
    if ((i = open("graphs", O_RDONLY)) == -1)
	die("Can't open graphs file");

    data = mmap(0, s.st_size, PROT_READ, MAP_SHARED, i, 0);
    if (data == MAP_FAILED)
	die("mmap() failed");

    if (close(i) == -1)
	die("close() failed");
    
    // brutally load our graphs
    for (n = 0, i = 0 ; i < GRAPHCOUNT ; i++) {
	memcpy(&graphs[i].dbc, &data[n], 2);
	n += 2;
	memcpy(&graphs[i].cbc, &data[n], 2);
	n += 2;
	graphs[i].graph = &data[n];
	n += (graphs[i].dbc * graphs[i].cbc) / 8;
	if ((graphs[i].dbc * graphs[i].cbc) % 8)
	    n++;
	//alert("Graph %d: %d x %d.", i+1, graphs[i].dbc, graphs[i].cbc);
	/*{
	    int j, p;
	    for (j = 0 ; j < graphs[i].dbc ; j++) {
		for (p = 0 ; p < graphs[i].cbc ; p++)
		    printf("%d", is_set(&graphs[i], j, p) ? 1 : 0);
		printf("\n");
	    }
	}*/
    }
}

int
is_set (struct graph *g, int db, int cb)
{
    int n = (db * g->cbc) + cb;
    return (g->graph[n / 8] << (n % 8)) & 128;
}

//=== insert ================================================================

void
insert (int c)
{
    char *hashes, *blocks;
    unsigned int i, j, datalength;
    unsigned int blocksize, len, hlen, dlen, clen;
    struct graph g;
    
    // read data length in bytes
    if (readall(c, &datalength, 4) != 4) {
	ioerror();
	return;
    }
    
    // find the graph for this datablock count
    blocksize = 64 * sqrt(datalength);
    if (datalength/blocksize > GRAPHCOUNT) {
	alert("I do not have a graph for %d data blocks!", datalength/blocksize);
	return;
    }
    g = graphs[datalength/blocksize-1];
    
    // allocate space for plaintext hash and data- and check-block hashes
    hlen = (1 + g.dbc + g.cbc) * HASHLEN;
    if (!(hashes = malloc(hlen)))
	die("malloc() failed");
    
    // padding
    while (g.dbc * blocksize < datalength)
	blocksize++;
    
    dlen = g.dbc * blocksize;
    clen = g.cbc * blocksize;
    len  = dlen + clen;
    
    // read data from client
    alert("Reading plaintext from client.");
    blocks = mbuf(len);
    memset(&blocks[i], 0, dlen - i);
    if (readall(c, blocks, datalength) != datalength) {
	ioerror();
	if (munmap(blocks, len) == -1)
	    die("munmap() failed");
	free(hashes);
	return;
    }
    
    // hash data
    alert("Hashing data.");
    sha_buffer(blocks, datalength, hashes);
    
    // generate check blocks
    alert("Generating %d check blocks for %d data blocks.", g.cbc, g.dbc);
    for (i = 0 ; i < g.cbc ; i++) {
	char b[1024];
	sprintf(b, "Check block %2d:", i+1);
	for (j = 0 ; j < g.dbc ; j++)
	    if (is_set(&g, j, i)) {
		xor(&blocks[dlen+(i*blocksize)], // check block (modified)
		    &blocks[j*blocksize], // data block (const)
		    blocksize);
		sprintf(b, "%s %d", b, j+1);
	    }
	alert("%s.", b);
    }
    
    alert("Hashing blocks.");
    
    // generate data block hashes
    for (i = 0 ; i < g.dbc ; i++)
	sha_buffer(&blocks[i*blocksize], blocksize, &hashes[(i+1)*HASHLEN]);
    
    // generate check block hashes
    for (i = 0 ; i < g.cbc ; i++)
	sha_buffer(&blocks[dlen+(i*blocksize)], blocksize,
		   &hashes[(g.dbc+1)*HASHLEN+(i*HASHLEN)]);
    
    // send the URI to the client
    alert("Writing key to client.");
    i = hlen + 4;
    if (writeall(c, &i, 4) != 4 ||
	writeall(c, &datalength, 4) != 4 ||
	writeall(c, hashes, hlen) != hlen) {
	ioerror();
	if (munmap(blocks, len) == -1)
	    die("munmap() failed");
	free(hashes);
	return;
    }

    // actually insert the blocks
    alert("Inserting %d blocks of %d bytes each.", g.dbc + g.cbc, blocksize);
    do_insert(blocks, NULL, g.dbc + g.cbc, blocksize, &hashes[HASHLEN]);
    alert("Insert complete.");
    
    if (munmap(blocks, len) == -1)
	die("munmap() failed");
    free(hashes);
}

int
hookup (const char hash[HASHLEN])
{
    struct sockaddr_in a;
    extern int errno;
    int c;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(ANARCAST_SERVER_PORT);
    a.sin_addr.s_addr = route(hash);

    if ((c = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	die("socket() failed");
    
    set_nonblock(c);

    // loop until connect works
    for (;;)
	if (connect(c, &a, sizeof(a)) == -1 && errno != EINPROGRESS) {
	    rmref(a.sin_addr.s_addr);
	    a.sin_addr.s_addr = route(hash);
	} else break;
    
    return c;
}

void
do_insert (const char *blocks, const char *mask, int blockcount, int blocksize, const char *hashes)
{
    int m, next, active;
    fd_set w;
    
    struct {
	int num;
	int off;
    } xfers[FD_SETSIZE];
    
    FD_ZERO(&w);
    next = active = 0;
    m = 1;
    
    for (;;) {
	int i;
	fd_set x = w;

	if (active) {
	    i = select(m, NULL, &x, NULL, NULL);
	    if (i == -1) die("select() failed");
	    if (!i) continue;
	}

	// make new connections
	while (active < CONCURRENCY && next < blockcount) {
	    int c;
	    // skip this part, its mask is true
	    if (mask && mask[next]) {
		next++;
		continue;
	    }
	    // connect to server, watch fd
	    c = hookup(&hashes[next*HASHLEN]);
	    FD_SET(c, &w);
	    if (c >= m) m = c + 1;
	    xfers[c].num = next;
	    xfers[c].off = -5; // 'i' + 4-byte datalength
	    active++;
	    next++;
	}

	// send data to eligible servers
	for (i = 0 ; i < m ; i++)
	    if (FD_ISSET(i, &x)) {
		int n = xfers[i].off;
		if (n == -5)
		    // command
		    n = writeall(i, "i", 1);
		else if (n < 0)
		    // data length
		    n = writeall(i, &(&blocksize)[4+n], -n);
		else
		    // data
		    n = writeall(i, &blocks[xfers[i].num*blocksize+n], blocksize-n);

		// io error
		if (n <= 0) {
		    int c;
		    ioerror();
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &w);
		    // connect to server, watch new fd
		    c = hookup(&hashes[xfers[i].num*HASHLEN]);
		    FD_SET(c, &w);
		    xfers[c].num = xfers[i].num;
		    xfers[c].off = -5; // 'i' + 4-byte datalength
		}
		
		// are we done?
		xfers[i].off += n;
		if (xfers[i].off == blocksize) {
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &w);
		    if (i == m) m--;
		    active--;
		}
	    }

	// nothing left to do?
	if (!active && next == blockcount)
	    break;
    }
}

//=== request ===============================================================

void
request (int c)
{
    int i;
    unsigned int datalength, blockcount, blocksize;
    char *blocks, *mask, hash[HASHLEN], *hashes;
    struct graph g;
    
    // read key length (a key is datalength + hashes)
    if (readall(c, &i, 4) != 4) {
	ioerror();
	return;
    }
    
    // is our key a series of > 2 hashes?
    i -= 4; // subtract datalength bytes
    if (i % HASHLEN || i == HASHLEN) {
	alert("Bad key length: %s.", i);
	return;
    }

    if (!(hashes = malloc(i)))
	die("malloc() failed");
    
    // read datalength and hashes from client
    if (readall(c, &datalength, 4) != 4 || readall(c, hashes, i) != i) {
	ioerror();
	free(hashes);
	return;
    }
    
    // find the graph for this datablock count
    blocksize = 64 * sqrt(datalength);
    if (datalength/blocksize > GRAPHCOUNT) {
	alert("I do not have a graph for %d data blocks!", datalength/blocksize);
	return;
    }
    g = graphs[datalength/blocksize-1];
    
    // padding
    while (g.dbc * blocksize < datalength)
	blocksize++;
    
    blockcount = g.dbc + g.cbc;
    mask = malloc(blockcount);
    blocks = mbuf(blockcount * blocksize);
    
    // slurp up all the data we can
    alert("Requesting %d blocks of %d bytes each.", blockcount, blocksize);
    memset(mask, 0, blockcount); // all parts are missing before we download them
    do_request(blocks, mask, blockcount, blocksize, &hashes[HASHLEN]);
    alert("Request complete.");
 
    // verify data
    alert("Verifying data integrity.");
    sha_buffer(blocks, datalength, hash);
    if (memcmp(hash, hashes, HASHLEN)) {
	alert("Decoding error: data does not verify!");
	goto out;
    }
    
    // write data to client
    if (writeall(c, &datalength, 4) != 4 || writeall(c, blocks, datalength) != datalength) {
	ioerror();
	goto out;
    }

out:
    if (munmap(blocks, blockcount * blocksize) == -1)
	die("munmap() failed");
    free(hashes);
    free(mask);
}

void
do_request (char *blocks, char *mask, int blockcount, int blocksize, const char *hashes)
{
    int m, next, active;
    fd_set r, w;
    
    struct {
	int num;
	int off;
	int dlen;
    } xfers[FD_SETSIZE];
    
    FD_ZERO(&r);
    FD_ZERO(&w);
    next = active = 0;
    m = 1;
    
    for (;;) {
	int i;
	fd_set s = r, x = w;

	if (active) {
	    i = select(m, &s, &x, NULL, NULL);
	    if (i == -1) die("select() failed");
	    if (!i) continue;
	}

	// make new connections
	while (active < CONCURRENCY && next < blockcount) {
	    int c;
	    // skip this part, its mask is true
	    if (mask && mask[next]) {
		next++;
		continue;
	    }
	    // connect to server, watch fd
	    c = hookup(&hashes[next*HASHLEN]);
	    FD_SET(c, &w);
	    if (c >= m) m = c + 1;
	    xfers[c].num = next;
	    xfers[c].off = -1; // 'r'
	    active++;
	    next++;
	}

	// send request to eligible servers
	for (i = 0 ; i < m ; i++)
	    if (FD_ISSET(i, &x)) {
		int n = xfers[i].off;
		if (n == -1)
		    // command
		    n = writeall(i, "r", 1);
		else
		    // hash
		    n = writeall(i, &hashes[xfers[i].num*HASHLEN+n], HASHLEN-n);

		// io error
		if (n <= 0) {
		    int c;
		    ioerror();
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &w);
		    // connect to server, watch new fd
		    c = hookup(&hashes[xfers[i].num*HASHLEN]);
		    FD_SET(c, &w);
		    xfers[c].num = xfers[i].num;
		    xfers[c].off = -1; // 'r'
		}
		
		// are we done sending our request?
		if ((xfers[i].off += n) == HASHLEN) {
		    FD_CLR(i, &w); // no more sending data...
		    FD_SET(i, &r); // reading is good fer yer brane!
		    xfers[i].off = -4; // datalength
		}
	    }

	// read our precious data
	for (i = 0 ; i < m ; i++)
	    if (FD_ISSET(i, &s)) {
		int n = xfers[i].off;
		if (n < 0)
		    // data length
		    n = readall(i, &(&xfers[i].dlen)[4+n], -n);
		else
		    // data
		    n = readall(i, &blocks[xfers[i].num*blocksize+n], blocksize-n);

		if (n <= 0) {
		    // the server hung up gracefully. request failed.
		    if (xfers[i].off == -4 && !n) {
			if (close(i) == -1)
			    die("close() failed");
			FD_CLR(i, &r);
			if (i == m) m--;
			active--;
		    } else { // io error, restart
			int c;
			ioerror();
			if (close(i) == -1)
			    die("close() failed");
			FD_CLR(i, &w);
			// connect to server, watch new fd
			c = hookup(&hashes[xfers[i].num*HASHLEN]);
			FD_SET(c, &w);
			xfers[c].num = xfers[i].num;
			xfers[c].off = -(1+HASHLEN); // 'r' + hash
		    }
		}

		// is the data length incorrect?
		xfers[i].off += n;
		if (!xfers[i].off && xfers[i].dlen != blocksize) {
		    alert("Data length read for block %d is incorrect! (%d != %d)", xfers[i].num+1, xfers[i].dlen, blocksize);
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &r);
		    if (i == m) m--;
		    active--;
		}

		// are we done reading the data?
		if (xfers[i].off == blocksize) {
		    char hash[HASHLEN];
		    sha_buffer(&blocks[xfers[i].num*blocksize], blocksize, hash);
		    if (memcmp(&hashes[xfers[i].num*HASHLEN], hash, HASHLEN))
			alert("Block %d is corrupt!", xfers[i].num+1);
		    else
			mask[xfers[i].num] = 1; // success
		    
		    if (close(i) == -1)
			die("close() failed");
		    FD_CLR(i, &r);
		    if (i == m) m--;
		    active--;
		}
	    }

	// nothing left to do?
	if (!active && next == blockcount)
	    break;
    }
}

//=== inform ================================================================

void
inform ()
{
    int c, n;
    struct sockaddr_in a;
    struct hostent *h;
    extern int h_errno;
    
    if (!(h = gethostbyname(inform_server))) {
	alert("%s: %s.", inform_server, hstrerror(h_errno));
	exit(1);
    }
    
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(INFORM_SERVER_PORT);
    a.sin_addr.s_addr = ((struct in_addr *)h->h_addr)->s_addr;
    
    if ((c = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	die("socket() failed");
    
    if (connect(c, &a, sizeof(a)) == -1)
	die("connect() failed");
    
    tree = NULL;
    
    // read and insert our friends
    for (n = 0 ; ; n++) {
	unsigned int i;
	int j = readall(c, &i, 4);
	if (!j) break;
    	if (j != 4) die("Inform server hung up unexpectedly");
	addref(i);
    }

    alert("%d Anarcast servers loaded.", n);

    if (!n) {
	puts("No servers, exiting.");
	exit(0);
    }
}

//=== routing ===============================================================

#define AVL_MAXHEIGHT 41

void avl_insert (struct node *node, struct node **stack[], int stackmax);
void avl_remove (struct node **stack[], int stackmax);
int avl_findwithstack (struct node **tree, struct node ***stack, int *count, const char hash[HASHLEN]);

void
refop (char op, char *hash, unsigned int addr)
{
    char hex[HASHLEN*2+1];
    struct in_addr x;
    
    // print our pretty message
    x.s_addr = addr;
    bytestohex(hex, hash, HASHLEN);
    alert("%c %15s %s", op, inet_ntoa(x), hex);
}

void
addref (unsigned int addr)
{
    int count;
    struct node *n;
    struct node **stack[AVL_MAXHEIGHT];
    
    if (!(n = malloc(sizeof(struct node))))
	die("malloc() failed");
    
    n->addr = addr;
    sha_buffer((char *) &addr, 4, n->hash);
    
    if (avl_findwithstack(&tree, stack, &count, n->hash))
	die("tried to addref() a duplicate reference");
    
    avl_insert(n, stack, count);

    refop('+', n->hash, addr);
}

void
rmref (unsigned int addr)
{
    int count;
    char hash[HASHLEN];
    struct node **stack[AVL_MAXHEIGHT];
    
    sha_buffer((char *) &addr, 4, hash);
    
    if (!avl_findwithstack(&tree, stack, &count, hash))
	die("tried to rmref() nonexistant reference");
    
    avl_remove(stack, count);

    refop('-', hash, addr);
}

unsigned int
route (const char hash[HASHLEN])
{
    int count;
    struct node **stack[AVL_MAXHEIGHT];
    
    avl_findwithstack(&tree, stack, &count, hash);
    
    if (count < 2)
	die("the tree is empty");
    
    refop('*', (*stack[count-2])->hash, (*stack[count-2])->addr);
    
    return (*stack[count-2])->addr;
}

int
avl_findwithstack (struct node **tree, struct node ***stack, int *count, const char hash[HASHLEN])
{
    struct node *n = *tree;
    int found = 0;
    
    *stack++ = tree;
    *count = 1;
    while (n) {
	int compval = memcmp(n->hash, hash, HASHLEN);
	if (compval < 0) {
	    (*count)++;
	    *stack++ = &n->left;
	    n = n->left;
	} else if (compval > 0) {
	    (*count)++;
	    *stack++ = &n->right;
	    n = n->right;
	} else {
	    found = 1;
	    break;
	}
    }
    
    return found;
}

// abandon all hope, ye who venture here

enum {TREE_BALANCED, TREE_LEFT, TREE_RIGHT};

static inline int
otherChild (int child)
{
    return child == TREE_LEFT ? TREE_RIGHT : TREE_LEFT;
}

static inline void
rotateWithChild (struct node **ptrnode, int child)
{
    struct node *node = *ptrnode;
    struct node *childnode;

    if (child == TREE_LEFT) {
        childnode = node->left;
        node->left = childnode->right;
        childnode->right = node;
    } else {
        childnode = node->right;
        node->right = childnode->left;
        childnode->left = node;
    }
    *ptrnode = childnode;

    if (childnode->heightdiff != TREE_BALANCED) {
        node->heightdiff = TREE_BALANCED;
        childnode->heightdiff = TREE_BALANCED;
    } else
        childnode->heightdiff = otherChild(child);
}

static inline void
rotateWithGrandChild (struct node **ptrnode, int child)
{
    struct node *node = *ptrnode;
    struct node *childnode;
    struct node *grandchildnode;
    int other = otherChild(child);

    if (child == TREE_LEFT) {
        childnode = node->left;
        grandchildnode = childnode->right;
        node->left = grandchildnode->right;
        childnode->right = grandchildnode->left;
        grandchildnode->left = childnode;
        grandchildnode->right = node;
    } else {
        childnode = node->right;
        grandchildnode = childnode->left;
        node->right = grandchildnode->left;
        childnode->left = grandchildnode->right;
        grandchildnode->right = childnode;
        grandchildnode->left = node;
    }
    *ptrnode = grandchildnode;

    if (grandchildnode->heightdiff == child) {
        node->heightdiff = other;
        childnode->heightdiff = TREE_BALANCED;
    } else if (grandchildnode->heightdiff == other) {
        node->heightdiff = TREE_BALANCED;
        childnode->heightdiff = child;
    } else {
        node->heightdiff = TREE_BALANCED;
        childnode->heightdiff = TREE_BALANCED;
    }
    grandchildnode->heightdiff = TREE_BALANCED;
}

void
avl_insert (struct node *node, struct node **stack[], int stackcount)
{
    int oldheightdiff = TREE_BALANCED;

    node->left = node->right = NULL;
    node->heightdiff = 0;
    *stack[--stackcount] = node;

    while (stackcount) {
        int nodediff, insertside;
        struct node *parent = *stack[--stackcount];

        if (parent->left == node)
            insertside = TREE_LEFT;
        else 
            insertside = TREE_RIGHT;

        node = parent;
        nodediff = node->heightdiff;

        if (nodediff == TREE_BALANCED) {
            node->heightdiff = insertside;
            oldheightdiff = insertside;
        } else if (nodediff != insertside) {
            node->heightdiff = TREE_BALANCED;
            return;
        } else {
            if (oldheightdiff == nodediff)
                rotateWithChild(stack[stackcount], insertside);
            else
                rotateWithGrandChild(stack[stackcount], insertside);
            return;
        }
    }
}

void
avl_remove (struct node **stack[], int stackcount)
{
    struct node *node = *stack[--stackcount];
    struct node *nextgreatest = node->left;
    struct node *relinknode;
    struct node **removenodeptr;

    if (nextgreatest) {
        int newmax = stackcount+1;
        struct node *next, tmp;

        while ((next = nextgreatest->right)) {
            newmax++;
            stack[newmax] = &nextgreatest->right;
            nextgreatest = next;
        }

        tmp.left = node->left;
	tmp.right = node->right;
	tmp.heightdiff = node->heightdiff;
	
	node->left = nextgreatest->left;
	node->right = nextgreatest->left;
	node->heightdiff = nextgreatest->heightdiff;
	
        nextgreatest->left = tmp.left;
	nextgreatest->right = tmp.right;
	nextgreatest->heightdiff = tmp.heightdiff;

        *stack[stackcount] = nextgreatest;
        stack[stackcount+1] = &nextgreatest->left;
        *stack[newmax] = node;
        stackcount = newmax;

        relinknode = node->left;
    } else
        relinknode = node->right;

    removenodeptr = stack[stackcount];

    while (stackcount) {
        int nodediff, removeside;
        struct node *parent = *stack[--stackcount];

        if (parent->left == node)
            removeside = TREE_LEFT;
        else 
            removeside = TREE_RIGHT;

        node = parent;
        nodediff = node->heightdiff;

        if (nodediff == TREE_BALANCED) {
            node->heightdiff = otherChild(removeside);
            break;
        } else if (nodediff == removeside) {
            node->heightdiff = TREE_BALANCED;
        } else {
            int childdiff;
            if (nodediff == TREE_LEFT)
                childdiff = node->left->heightdiff;
            else
                childdiff = node->right->heightdiff;

            if (childdiff == otherChild(nodediff))
                rotateWithGrandChild(stack[stackcount], nodediff);
            else {
                rotateWithChild(stack[stackcount], nodediff);
                if (childdiff == TREE_BALANCED)
                    break;
            }

            node = *stack[stackcount];
        }
    }

    *removenodeptr = relinknode;
}


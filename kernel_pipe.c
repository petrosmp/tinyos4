#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_cc.h"

static file_ops reader_file_ops = {
	.Read = pipe_read,
	.Write = no_op_write,
	.Close = pipe_reader_close
};

static file_ops writer_file_ops = {
	.Read = no_op_read,
	.Write = pipe_write,
	.Close = pipe_writer_close
};

c_node* init_list(int size, const char *data){

    // allocate the first node
    c_node *old = (c_node*) xmalloc(sizeof(c_node));

    // initialize the fields of the first node
    old->c = (data!=NULL) ? data[0] : '\0';
    old->prev = NULL;
    old->next = NULL;

    // save the head to return it
    c_node *list = old;

    // create the rest of the nodes
    for(int i=1; i<size; i++){
        // allocate the new node
        c_node *new_node = (c_node*) malloc(sizeof(c_node));

        // initialize the fields of the new node
        new_node->next = (old->next!=NULL) ? old -> next : old;
        new_node->prev = old;
        new_node->prev->next = new_node;
        new_node->next->prev = new_node;
        new_node->c = (data!=NULL) ? data[i] : '\0';

        // set old to the new node
        old = new_node;
    }

    return list;
}

c_node* get_empty_node(c_node* list){

    c_node *tmp = list->next;
    while(tmp!=list){
        if(tmp->c=='\0'){
            return tmp;
        }
    }
    return list->c=='\0' ? list : NULL;
}

pipe_cb* init_pipe_obj(){
    
    pipe_cb *pipe_obj = (pipe_cb*) xmalloc(sizeof(pipe_cb));
    pipe_obj->BUFFER = init_list(PIPE_BUFFER_SIZE, NULL);
    pipe_obj->has_data = COND_INIT;
    pipe_obj->has_space = COND_INIT;
    pipe_obj->r_position = pipe_obj->BUFFER;
    pipe_obj->w_position = pipe_obj->BUFFER;
    pipe_obj->written_bytes = 0;

    return pipe_obj;
}

int sys_Pipe(pipe_t* pipe){

    // get 2 fcbs and tids with reserve
    FCB* fcbs[2];
    Fid_t fids[2];

    // Fail if FCB reservation fails
    if(!FCB_reserve(2, fids, fcbs)){
        return -1;
    }

    // create the pipe object
    pipe_cb *pipe_obj = init_pipe_obj();
    if(pipe_obj  == NULL){
        return -1;                      // fail if pipe initialization fails
    }

    // fid[0] is the read end, fid[1] is the write end
    pipe->read = fids[0];
    pipe->write = fids[1];

    // setup the reader (fcbs[0] is the FCB at which fids[0] is pointing)
    fcbs[0]->streamfunc = &reader_file_ops;
    fcbs[0]->streamobj = pipe_obj;
    // freelist node is already initiated by initialize_files() (kernel_streams.c line 21)
    // refcount is already incremented by FCB_reserve() (kernel_streams.c line 94)
    pipe_obj->reader = fcbs[0];

    // setup the writer (fcbs[1] is the FCB at which fids[1] is pointing)
    fcbs[1]->streamfunc = &writer_file_ops;
    fcbs[1]->streamobj = pipe_obj;
    // freelist node is already initiated by initialize_files() (kernel_streams.c line 21)
    // refcount is already incremented by FCB_reserve() (kernel_streams.c line 94)
    pipe_obj->writer = fcbs[1];

    return 0;
}

int pipe_write(void* pipecb_t, const char *buf, unsigned int n){

    pipe_cb* pipe_to_write_to = (pipe_cb*) pipecb_t; 

    // check if pipe and writer exist and are open
    if(pipe_to_write_to == NULL || pipe_to_write_to->writer == NULL){
        return -1;
    }

    // check if reader is closed
    if(pipe_to_write_to->reader == NULL){
        return -1;
    }

    // check if there is available space
    int availableSpace = PIPE_BUFFER_SIZE - pipe_to_write_to->written_bytes;
    while(availableSpace == 0 && pipe_to_write_to->reader != NULL){
        //kernel_broadcast(&pipe_to_write_to->has_data);          // signal the waiting readers to consume some data and free up space
        kernel_wait(&pipe_to_write_to->has_space, SCHED_PIPE);   // wait till there is some free space
        availableSpace = PIPE_BUFFER_SIZE - pipe_to_write_to->written_bytes;        // after waking up re-calculate the available space (remove this and watch tests timeout)
    }

    // once we wake up, re-check if reader is closed
    if(pipe_to_write_to->reader == NULL){
        return -1;
    }

    // if n fits in the available space write n, else write as much as possible
    int bytes_to_write = (n<availableSpace) ? n : availableSpace;

    // write
    for(int i=0; i<bytes_to_write; i++){
		if(pipe_to_write_to->w_position->c != '\0'){	// avoid overwritting non-null characters
			return -1;
		}
        pipe_to_write_to->w_position->c = buf[i];   // write
        pipe_to_write_to->w_position = pipe_to_write_to->w_position->next; // move cursor
        pipe_to_write_to->written_bytes++;          // increment written bytes
    }

    // broadcast the waiting readers
    kernel_broadcast(&pipe_to_write_to->has_data);

    return bytes_to_write;  // return the number of bytes writen
}

int pipe_read(void* pipecb_t, char *buf, unsigned int n){
    pipe_cb* pipe_to_read_from = (pipe_cb*) pipecb_t; 

    // check if pipe and reader exist and are open
    if(pipe_to_read_from == NULL || pipe_to_read_from->reader == NULL){
        return -1;
    }

    // check if writer is closed (doesn't matter unless writtenBytes is also 0)
    if(pipe_to_read_from->writer == NULL && pipe_to_read_from->written_bytes == 0){
        return 0;   // vlepe front
    }

    // check if there are available data to read
    while(pipe_to_read_from->written_bytes == 0 && pipe_to_read_from->writer != NULL){
        //kernel_broadcast(&pipe_to_read_from->has_space);        // wake up the blocked writers to create some data
        kernel_wait(&pipe_to_read_from->has_data, SCHED_PIPE);  // wait till there are some data EDW KOLLAEI, GIATI WRITER != NULL ALLA THA PREPE NA EINAI
    }

    // when we wake up, re-check if writer is closed
    if(pipe_to_read_from->writer == NULL && pipe_to_read_from->written_bytes == 0){
        return 0;   // vlepe front
    }

    // if n bytes are present in the pipe read n, else read as many as possible
    int bytes_to_read = (n<pipe_to_read_from->written_bytes) ? n : pipe_to_read_from->written_bytes;

    // read
    for(int i=0; i<bytes_to_read; i++){
        buf[i] = pipe_to_read_from->r_position->c;      // read
		pipe_to_read_from->r_position->c = '\0';
        pipe_to_read_from->r_position = pipe_to_read_from->r_position->next;    // move cursor
        pipe_to_read_from->written_bytes--;             // decrement write counter
    }

    // broadcast the waiting writers
    kernel_broadcast(&pipe_to_read_from->has_space);

    return bytes_to_read;       // return the number of bytes read
}

int pipe_writer_close(void* _pipecb){

    pipe_cb *pipe_to_close = (pipe_cb*) _pipecb;
    
    // check if the pipe and the writer exist
    if(pipe_to_close == NULL || pipe_to_close->writer == NULL){
        return -1;
    }

    pipe_to_close->writer = NULL;           // free would deallocate the FCB's memory, so we just set the pointer to null
    
    if (pipe_to_close->reader == NULL){     // if reader is closed too, deallocate the buffer and the pipe itself
        free(pipe_to_close->BUFFER);        // has to be done with a dedicated free_list func TODO
        free(pipe_to_close);
    }
    else{
    	kernel_broadcast(&pipe_to_close->has_data); // else broadcast to hasData (so any waiting readers wake up and finish reading the data)
    }
    return 0; 

}

int pipe_reader_close(void* _pipecb){
    pipe_cb *pipe_to_close = (pipe_cb*) _pipecb;
    
    // check if the pipe and the reader exist
    if(pipe_to_close == NULL || pipe_to_close->reader == NULL){
        return -1;
    }

    pipe_to_close->reader = NULL;           // free would deallocate the FCB's memory, so we just set the pointer to null
    
    if (pipe_to_close->writer == NULL){     // if writer is closed too, deallocate the buffer and the pipe itself
        free(pipe_to_close->BUFFER);        // has to be done with a dedicated free_list func TODO
        free(pipe_to_close);
    }
    else{
    	kernel_broadcast(&pipe_to_close->has_space); // else broadcast to hasSpace (so any waiting writers wake up and exit too)
    }

    return 0;
}

int no_op_read(void* pipecb_t, char *buf, unsigned int n){
    return -1;
}

int no_op_write(void* pipecb_t, const char *buf, unsigned int n){
    return -1;
}

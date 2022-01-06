#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"

#if OPT_A2
#include <syscall.h>
#include <mips/trapframe.h>
#include <synch.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <kern/signal.h>

int sys_fork(struct trapframe *tf, int *retval) {
  struct proc *curprocess = curproc;

  struct proc *child = proc_create_runprogram("newprocess");
  if (child == NULL) {
    //unable to create process
    //panic("unable to create process\n");
    return EMPROC;
  }

  // create and copy address space for child
  int err = as_copy(curprocess->p_addrspace, &child->p_addrspace);        //curproc_setas() as a guide 
  if (err) {
    // something bad happened during as_copy
    return err;
  }

  child->parent = curproc;
  curproc->children++;
  //array_add(curproc->child,&child->p_id, NULL);

  struct trapframe *tfTemp = (struct trapframe *)kmalloc(sizeof(struct trapframe));
	if (tfTemp == NULL) {
    return 1;
	}
  *tfTemp = *tf;

  // create thread
  int errt = thread_fork("child thread", child, enter_forked_process, (void *)tfTemp, (unsigned long)NULL);           // modify enter_forked
  if (errt) {
    panic("child first thread fork error\n");
  }
  *retval = child->p_id;
  return 0;
}


  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {
  
  struct addrspace *as;
  struct proc *p = curproc;
  

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  lock_acquire(proc_lock);
  lock_acquire(p->lock);
  p->es = true;
  p->ec = _MKWAIT_EXIT(exitcode);

  if (exitcode == SIGTERM) {
    p->ec = _MKWAIT_SIG(exitcode);
  }
  
  struct proc *child_p;
  if (p->children != 0) {
    for (unsigned int i = 0; i < 64; ++i) {
        child_p = p_array[i];
        if (child_p!=NULL && child_p->parent == p) {

            if (child_p->es == true) {
              as = child_p->p_addrspace;
              child_p->p_addrspace = NULL;
              //as_deactivate();
              as_destroy(as);
              proc_destroy(child_p);
              p_array[i] = NULL;
            } else {
              child_p->parent = NULL;

            }
            p->children--;
            //array_remove(p->child,i);
        }
    }
    
  }

  if (p->parent == NULL) {

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);
  lock_release(p->lock);
  lock_release(proc_lock);
  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);


  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  thread_exit();

  } else {
    cv_signal(p->cv,p->lock);
    lock_release(p->lock);
    lock_release(proc_lock);
  
    proc_remthread(curthread);
    thread_exit();
  }
}



/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = curproc->p_id;
  return 0;
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;
  struct addrspace *as;
  
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

/*
  if (curproc->child == NULL) return 3;
  size = array_num(curproc->child);
  for (int i = 0; i < size; ++i) {
    temp = array_get(curproc->child,i);
    if (*temp == (int)pid) {
        location = i;
        break;
    }
    
    if (i == size) return 3;
  }
  */
  if (curproc->children == 0) return 3;
  struct proc *p;
  p = p_array[(int)pid];
  if (p == NULL) return 3;
  if (p->parent != curproc) return 3;
  lock_acquire(p->lock);
  if (p->es == false) {
    cv_wait(p->cv, p->lock);
  } 

  if (options != 0) {
    return(EINVAL);
  }
  
  exitstatus = p->ec;
  lock_release(p->lock);

  as = p->p_addrspace;
  //as_deactivate();
  p->p_addrspace = NULL;
  as_destroy(as);
  proc_destroy(p);

  lock_acquire(curproc->lock);
  //array_remove(curproc->child, location);
  curproc->children--;
  lock_release(curproc->lock);

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}





int sys_execv(const_userptr_t progname, userptr_t args) {

  struct addrspace *as;
	struct addrspace *oldas;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;


 // get program name
  int maxLen = 128;
  char progKern[maxLen];
  size_t actual;
  result = copyinstr(progname, progKern, maxLen, &actual);
  if (result) {
		return result;
	}

  // count args
  int nargs = 0;
  int i = 0;
  int argsptr[nargs];

  while (*(char **)(args+i) != NULL) {
    result = copyin((args+i), &argsptr[i], 4);
    if (result) {
		  return EINVAL;
	  }

    nargs++;
    i += 4;
  }

  // copy in each string
  char *argsKern[nargs];

  for (int i = 0; i < nargs; ++i) {
    argsKern[i] = kmalloc(maxLen*sizeof(char));

    result = copyinstr((userptr_t)argsptr[i], argsKern[i],maxLen,&actual);
    if (result) {
		  return EINVAL;
	  }
    //kprintf(argsKern[i]);
  }


	/* Open the file. */
	result = vfs_open(progKern, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	oldas = curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

  as_destroy(oldas);

	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}



















#else 
// old code
void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}
#endif


#include "qp_solver.h"
#include "common.h"
#include "example.h"
#include "classifier.h"

// $Id: qp_solver.cc,v 1.7 2001/01/16 20:13:06 taku-ku Exp $;

namespace TinySVM {

// swap i and j
void
QP_Solver::swap_index(const int i, const int j)
{
  swap(y[i],            y[j]);
  swap(x[i],            x[j]);
  swap(alpha[i],        alpha[j]);
  swap(status[i],       status[j]);
  swap(G[i],            G[j]);
  swap(b[i],            b[j]);
  swap(shrink_iter[i],  shrink_iter[j]);
  swap(active2index[i], active2index[j]);
}

int
QP_Solver::solve(const BaseExample &e,
		 const Param &p,
		 double *_b, double *_alpha, double *_G, 
		 double &rho, double &obj)
{
  try {
    param        = p;
    C            = p.C;
    eps          = p.eps;
    shrink_size  = p.shrink_size;
    shrink_eps   = p.shrink_eps;
    final_check  = p.final_check;
    l            = e.l;
    active_size  = l;
    iter         = 0;
    hit_old      = 0;

    clone(alpha, _alpha, l);
    clone(G, _G, l);
    clone(b, _b, l);
    clone(y, e.y, l);
    clone(x, e.x, l);

    q_matrix = new QMatrix(e,p);
    q_matrix->x = x;
    q_matrix->y = y;

    shrink_iter  = new int [l];
    status       = new int [l];
    active2index = new int [l];

    for (int i = 0; i < l; i++) {
      status[i] = alpha2status(alpha[i]);
      shrink_iter[i]  = 0;
      active2index[i] = i;
    }

    for (;;) {
      learn_sub();
      if (final_check == 0 || check_inactive () == 0)  break; 
      q_matrix->rebuildCache(active_size);
      q_matrix->x = x;
      q_matrix->y = y;
      shrink_eps = p.shrink_eps;
    }

    // make result
    for (int i = 0; i < l; i++) {
      _alpha[active2index[i]] = alpha[i];
      _G[active2index[i]]     = G[i];
    }

    // calculate objective value
    obj = 0;
    for (int i = 0; i < l; i++) obj += alpha[i] * (G[i] + b[i]);
    obj /= 2;

    // calculate threshold b
    rho = lambda_eq;

    delete [] status;
    delete [] alpha;
    delete [] x;
    delete [] y;
    delete [] b;
    delete [] G;
    delete [] active2index;
    delete [] shrink_iter;
    delete q_matrix;

    fprintf (stdout, "\nDone! %d iterations\n\n", iter);
    return 1;
  }

  catch (...) {
    fprintf (stderr, "QP_Solver::learn(): Out of memory\n");
    exit (EXIT_FAILURE);
  }
}

void
QP_Solver::learn_sub()
{
  fprintf (stdout, "%6d examples, cache size: %d\n", 
	   active_size, q_matrix->size);

  while(++iter) {
    /////////////////////////////////////////////////////////
    // Select Working set
    double Gmax1 = -INF;
    int i = -1;
    double Gmax2 = -INF;
    int j = -1;

    for (int k = 0; k < active_size; k++) {
     if (y[k] > 0) {
	if (!is_upper_bound (k)) {
	  if (-G[k] > Gmax1) {
	    Gmax1 = -G[k];
	    i = k;
	  }
	}

	if (!is_lower_bound (k)) {
	  if (G[k] > Gmax2) {
	    Gmax2 = G[k];
	    j = k;
	  }
	}
      } else {
	if (!is_upper_bound (k)) {
	  if (-G[k] > Gmax2) {
	    Gmax2 = -G[k];
	    j = k;
	  }
	}

	if (!is_lower_bound (k)) {
	  if (G[k] > Gmax1) {
	    Gmax1 = G[k];
	    i = k;
	  }
	}
      }
    }

    /////////////////////////////////////////////////////////
    //
    // Solving QP sub problems
    //
    double old_alpha_i = alpha[i];
    double old_alpha_j = alpha[j];

    double *Q_i = q_matrix->getQ (i, active_size);
    double *Q_j = q_matrix->getQ (j, active_size);

    if (y[i] * y[j] < 0) {
      double L = max (0.0, alpha[j] - alpha[i]);
      double H = min (C, C + alpha[j] - alpha[i]);
      alpha[j] += (-G[i] - G[j]) / (Q_i[i] + Q_j[j] + 2 * Q_i[j]);
      if (alpha[j] >= H)      alpha[j] = H;
      else if (alpha[j] <= L) alpha[j] = L;
      alpha[i] += (alpha[j] - old_alpha_j);
    } else {
      double L = max (0.0, alpha[i] + alpha[j] - C);
      double H = min (C, alpha[i] + alpha[j]);
      alpha[j] += (G[i] - G[j]) / (Q_i[i] + Q_j[j] - 2 * Q_i[j]);
      if (alpha[j] >= H)      alpha[j] = H;
      else if (alpha[j] <= L) alpha[j] = L;
      alpha[i] -= (alpha[j] - old_alpha_j);
    }

    /////////////////////////////////////////////////////////
    //
    // update status
    // 
    status[i] = alpha2status(alpha[i]);
    status[j] = alpha2status(alpha[j]);

    double delta_alpha_i = alpha[i] - old_alpha_i;
    double delta_alpha_j = alpha[j] - old_alpha_j;

    /////////////////////////////////////////////////////////
    //
    // Update O and Calculate \lambda^eq for shrinking, Calculate lambda^eq,
    // (c.f. Advances in Kernel Method pp.175)
    // lambda_eq = 1/|FREE| \sum_{i \in FREE} y_i - \sum_{l} y_i \alpha_i k(x_i,x_j) (11.29)
    //
    lambda_eq = 0.0;
    int size_A = 0;
    for (int k = 0; k < active_size; k++) {
      G[k] += Q_i[k] * delta_alpha_i + Q_j[k] * delta_alpha_j;
      if (is_free (k)) {
	lambda_eq -= G[k] * y[k];
	size_A++;
      }
    }

    /////////////////////////////////////////////////////////
    //
    // Select example for shrinking,
    // (c.f. 11.5 Efficient Implementation in Advances in Kernel Method pp. 175)
    //
    lambda_eq = size_A ? (lambda_eq / size_A) : 0.0;
    double kkt_violation = 0.0;

    for (int k = 0; k < active_size; k++) {
      double lambda_up = -(G[k] + y[k] * lambda_eq);	// lambda_lo = -lambda_up

      // termination criteria (11.32,11.33,11.34)
      if (! is_lower_bound (k) && lambda_up < -kkt_violation) kkt_violation = -lambda_up;
      if (! is_upper_bound (k) && lambda_up >  kkt_violation) kkt_violation =  lambda_up;

      // "If the estimate (11.30) or (11.31) was positive (or above some threshold) at
      // each of the last h iterations, it is likely that this will be true at the  optimal solution" 
      // lambda^up  (11.30) lambda^low = lambda^up * status[k]
      if (lambda_up * status[k] > shrink_eps) {
	if (shrink_iter[k]++ > shrink_size) {
	  active_size--;
	  swap_index(k, active_size); // remove this data from working set
	  q_matrix->swap_index(k, active_size);
	  q_matrix->update(active_size);
	  k--;
	}
      } else {
	// reset iter, if current data dose not satisfy the condition (11.30), (11.31)
	shrink_iter[k] = 0;
      }
    }
    
    /////////////////////////////////////////////////////////
    //
    // Check terminal criteria, show information of iteration
    //
    if (kkt_violation < eps) break;

    if ((iter % 50) == 0) { fprintf (stdout, "."); fflush (stdout); };

    if ((iter % 1000) == 0) {
      fprintf (stdout, " %6d %6d %5d %1.4f %5.1f%% %5.1f%%\n",
	       iter, active_size, q_matrix->size, kkt_violation,
	       100.0 * (q_matrix->hit - hit_old)/2000,
	       100.0 * q_matrix->hit/(q_matrix->hit + q_matrix->miss));
      fflush (stdout);

      // save old hit rate
      hit_old = q_matrix->hit;

      // This shrink eps rule is delivered from svm_light.
      shrink_eps = shrink_eps * 0.7 + kkt_violation * 0.3;
    }
  }
}

int
QP_Solver::check_inactive ()
{
  // final check
  fprintf (stdout, "\nChecking optimality of inactive variables ");
  fflush (stdout);

  // make dummy classifier
  try {
    Model *tmp_model = new Model (param);
    tmp_model->ref ();
    tmp_model->b = -lambda_eq;
    for (int i = 0; i < l; i++) {
      if (! is_lower_bound (i))	tmp_model->add (alpha[i] * y[i], (feature_node *) x[i]);
    }

    int react_num = 0;
    for (int k= l-1; k >= active_size; k--) {
      double lambda_up = 1 - y[k] * tmp_model->classify (x[k]);

      // Oops!, must be added to the active example.
      if ( (! is_lower_bound (k) && lambda_up < -eps) ||
	   (! is_upper_bound (k) && lambda_up >  eps) ) {
	swap_index(k, active_size);
	active_size++;
	++k;
      }
    }

    delete tmp_model;
    fprintf (stdout, " re-activated: %d\n", react_num);
    
    return react_num;
  }

  catch (...) {
    fprintf (stderr, "QP_Solver::check_inactive(): Out of memory\n");
    exit (EXIT_FAILURE);
  }
}

}
#include "parsa.h"
namespace PS {
App* App::create(const string& name, const string& conf) {
  auto my_role = Postoffice::instance().myNode().role();
  if (my_role == Node::SCHEDULER) {
    return new ParsaScheduler(conf);
  } else if (my_role == Node::WORKER) {
    return new ParsaWorker(conf);
  } else if (my_role == Node::SERVER) {
    return new ParsaServer(conf);
  }
  return NULL;
}
} // namespace PS

int main(int argc, char *argv[]) {
  PS::Postoffice::instance().start(&argc, &argv);;
  PS::Postoffice::instance().stop();
  return 0;
}

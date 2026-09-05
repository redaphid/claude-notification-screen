// Serves web/phone/ as static assets. A real fetch handler (not a Pages
// static site) so later stages can add server-side logic - e.g. stage 2b's
// fake BLE sink logging frames - without a second deploy target.
export default {
  async fetch(request, env) {
    return env.ASSETS.fetch(request);
  },
};

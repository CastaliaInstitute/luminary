const LIVE_PREFIX = '/cliffhouse/runtime/';
const LIVE_ORIGIN = 'https://raw.githubusercontent.com/CastaliaInstitute/luminary/runtime-live/';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (!url.pathname.startsWith(LIVE_PREFIX)) {
      return env.ASSETS.fetch(request);
    }

    const relativePath = url.pathname.slice(1);
    const upstream = new URL(relativePath, LIVE_ORIGIN);
    // GitHub's raw-file edge may briefly retain a force-updated branch URL.
    // A five-minute generation key bounds that delay without producing an
    // unbounded cache key for every browser request.
    upstream.searchParams.set('generation', String(Math.floor(Date.now() / 300000)));
    const response = await fetch(upstream, {
      headers: { Accept: request.headers.get('Accept') || '*/*' },
      redirect: 'follow',
    });
    if (!response.ok) {
      return new Response('Live Cliff House runtime is temporarily unavailable.', {
        status: response.status,
        headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' },
      });
    }

    const headers = new Headers(response.headers);
    headers.set('Cache-Control', 'public, max-age=60, stale-while-revalidate=240');
    headers.set('X-Luminary-Runtime', 'runtime-live');
    return new Response(response.body, { status: response.status, headers });
  },
};

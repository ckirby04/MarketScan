import ChatPage from "@/components/ChatPage";

// A conversation, as the service keeps it.
export default async function Page({ params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  return <ChatPage id={id} />;
}
